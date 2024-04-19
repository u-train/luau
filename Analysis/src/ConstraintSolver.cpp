// This file is part of the Luau programming language and is licensed under MIT License; see LICENSE.txt for details

#include "Luau/Anyification.h"
#include "Luau/ApplyTypeFunction.h"
#include "Luau/Common.h"
#include "Luau/ConstraintSolver.h"
#include "Luau/DcrLogger.h"
#include "Luau/Instantiation.h"
#include "Luau/Instantiation2.h"
#include "Luau/Location.h"
#include "Luau/ModuleResolver.h"
#include "Luau/OverloadResolution.h"
#include "Luau/Quantify.h"
#include "Luau/RecursionCounter.h"
#include "Luau/Simplify.h"
#include "Luau/TableLiteralInference.h"
#include "Luau/TimeTrace.h"
#include "Luau/ToString.h"
#include "Luau/Type.h"
#include "Luau/TypeFamily.h"
#include "Luau/TypeFwd.h"
#include "Luau/TypeUtils.h"
#include "Luau/Unifier2.h"
#include "Luau/VecDeque.h"
#include "Luau/VisitType.h"
#include <algorithm>
#include <utility>

LUAU_FASTFLAGVARIABLE(DebugLuauLogSolver, false);
LUAU_FASTFLAGVARIABLE(DebugLuauLogBindings, false);
LUAU_FASTINTVARIABLE(LuauSolverRecursionLimit, 500);

namespace Luau
{

size_t HashBlockedConstraintId::operator()(const BlockedConstraintId& bci) const
{
    size_t result = 0;

    if (const TypeId* ty = get_if<TypeId>(&bci))
        result = std::hash<TypeId>()(*ty);
    else if (const TypePackId* tp = get_if<TypePackId>(&bci))
        result = std::hash<TypePackId>()(*tp);
    else if (Constraint const* const* c = get_if<const Constraint*>(&bci))
        result = std::hash<const Constraint*>()(*c);
    else
        LUAU_ASSERT(!"Should be unreachable");

    return result;
}

[[maybe_unused]] static void dumpBindings(NotNull<Scope> scope, ToStringOptions& opts)
{
    for (const auto& [k, v] : scope->bindings)
    {
        auto d = toString(v.typeId, opts);
        printf("\t%s : %s\n", k.c_str(), d.c_str());
    }

    for (NotNull<Scope> child : scope->children)
        dumpBindings(child, opts);
}

// used only in asserts
[[maybe_unused]] static bool canMutate(TypeId ty, NotNull<const Constraint> constraint)
{
    if (auto blocked = get<BlockedType>(ty))
        return blocked->getOwner() == constraint;

    return true;
}

// used only in asserts
[[maybe_unused]] static bool canMutate(TypePackId tp, NotNull<const Constraint> constraint)
{
    if (auto blocked = get<BlockedTypePack>(tp))
        return blocked->owner == nullptr || blocked->owner == constraint;

    return true;
}

static std::pair<std::vector<TypeId>, std::vector<TypePackId>> saturateArguments(TypeArena* arena, NotNull<BuiltinTypes> builtinTypes,
    const TypeFun& fn, const std::vector<TypeId>& rawTypeArguments, const std::vector<TypePackId>& rawPackArguments)
{
    std::vector<TypeId> saturatedTypeArguments;
    std::vector<TypeId> extraTypes;
    std::vector<TypePackId> saturatedPackArguments;

    for (size_t i = 0; i < rawTypeArguments.size(); ++i)
    {
        TypeId ty = rawTypeArguments[i];

        if (i < fn.typeParams.size())
            saturatedTypeArguments.push_back(ty);
        else
            extraTypes.push_back(ty);
    }

    // If we collected extra types, put them in a type pack now. This case is
    // mutually exclusive with the type pack -> type conversion we do below:
    // extraTypes will only have elements in it if we have more types than we
    // have parameter slots for them to go into.
    if (!extraTypes.empty() && !fn.typePackParams.empty())
    {
        saturatedPackArguments.push_back(arena->addTypePack(extraTypes));
    }

    for (size_t i = 0; i < rawPackArguments.size(); ++i)
    {
        TypePackId tp = rawPackArguments[i];

        // If we are short on regular type saturatedTypeArguments and we have a single
        // element type pack, we can decompose that to the type it contains and
        // use that as a type parameter.
        if (saturatedTypeArguments.size() < fn.typeParams.size() && size(tp) == 1 && finite(tp) && first(tp) && saturatedPackArguments.empty())
        {
            saturatedTypeArguments.push_back(*first(tp));
        }
        else if (saturatedPackArguments.size() < fn.typePackParams.size())
        {
            saturatedPackArguments.push_back(tp);
        }
    }

    size_t typesProvided = saturatedTypeArguments.size();
    size_t typesRequired = fn.typeParams.size();

    size_t packsProvided = saturatedPackArguments.size();
    size_t packsRequired = fn.typePackParams.size();

    // Extra types should be accumulated in extraTypes, not saturatedTypeArguments. Extra
    // packs will be accumulated in saturatedPackArguments, so we don't have an
    // assertion for that.
    LUAU_ASSERT(typesProvided <= typesRequired);

    // If we didn't provide enough types, but we did provide a type pack, we
    // don't want to use defaults. The rationale for this is that if the user
    // provides a pack but doesn't provide enough types, we want to report an
    // error, rather than simply using the default saturatedTypeArguments, if they exist. If
    // they did provide enough types, but not enough packs, we of course want to
    // use the default packs.
    bool needsDefaults = (typesProvided < typesRequired && packsProvided == 0) || (typesProvided == typesRequired && packsProvided < packsRequired);

    if (needsDefaults)
    {
        // Default types can reference earlier types. It's legal to write
        // something like
        // type T<A, B = A> = (A, B) -> number
        // and we need to respect that. We use an ApplyTypeFunction for this.
        ApplyTypeFunction atf{arena};

        for (size_t i = 0; i < typesProvided; ++i)
            atf.typeArguments[fn.typeParams[i].ty] = saturatedTypeArguments[i];

        for (size_t i = typesProvided; i < typesRequired; ++i)
        {
            TypeId defaultTy = fn.typeParams[i].defaultValue.value_or(nullptr);

            // We will fill this in with the error type later.
            if (!defaultTy)
                break;

            TypeId instantiatedDefault = atf.substitute(defaultTy).value_or(builtinTypes->errorRecoveryType());
            atf.typeArguments[fn.typeParams[i].ty] = instantiatedDefault;
            saturatedTypeArguments.push_back(instantiatedDefault);
        }

        for (size_t i = 0; i < packsProvided; ++i)
        {
            atf.typePackArguments[fn.typePackParams[i].tp] = saturatedPackArguments[i];
        }

        for (size_t i = packsProvided; i < packsRequired; ++i)
        {
            TypePackId defaultTp = fn.typePackParams[i].defaultValue.value_or(nullptr);

            // We will fill this in with the error type pack later.
            if (!defaultTp)
                break;

            TypePackId instantiatedDefault = atf.substitute(defaultTp).value_or(builtinTypes->errorRecoveryTypePack());
            atf.typePackArguments[fn.typePackParams[i].tp] = instantiatedDefault;
            saturatedPackArguments.push_back(instantiatedDefault);
        }
    }

    // If we didn't create an extra type pack from overflowing parameter packs,
    // and we're still missing a type pack, plug in an empty type pack as the
    // value of the empty packs.
    if (extraTypes.empty() && saturatedPackArguments.size() + 1 == fn.typePackParams.size())
    {
        saturatedPackArguments.push_back(arena->addTypePack({}));
    }

    // We need to have _something_ when we substitute the generic saturatedTypeArguments,
    // even if they're missing, so we use the error type as a filler.
    for (size_t i = saturatedTypeArguments.size(); i < typesRequired; ++i)
    {
        saturatedTypeArguments.push_back(builtinTypes->errorRecoveryType());
    }

    for (size_t i = saturatedPackArguments.size(); i < packsRequired; ++i)
    {
        saturatedPackArguments.push_back(builtinTypes->errorRecoveryTypePack());
    }

    // At this point, these two conditions should be true. If they aren't we
    // will run into access violations.
    LUAU_ASSERT(saturatedTypeArguments.size() == fn.typeParams.size());
    LUAU_ASSERT(saturatedPackArguments.size() == fn.typePackParams.size());

    return {saturatedTypeArguments, saturatedPackArguments};
}

bool InstantiationSignature::operator==(const InstantiationSignature& rhs) const
{
    return fn == rhs.fn && arguments == rhs.arguments && packArguments == rhs.packArguments;
}

size_t HashInstantiationSignature::operator()(const InstantiationSignature& signature) const
{
    size_t hash = std::hash<TypeId>{}(signature.fn.type);
    for (const GenericTypeDefinition& p : signature.fn.typeParams)
    {
        hash ^= (std::hash<TypeId>{}(p.ty) << 1);
    }

    for (const GenericTypePackDefinition& p : signature.fn.typePackParams)
    {
        hash ^= (std::hash<TypePackId>{}(p.tp) << 1);
    }

    for (const TypeId a : signature.arguments)
    {
        hash ^= (std::hash<TypeId>{}(a) << 1);
    }

    for (const TypePackId a : signature.packArguments)
    {
        hash ^= (std::hash<TypePackId>{}(a) << 1);
    }

    return hash;
}

void dump(ConstraintSolver* cs, ToStringOptions& opts)
{
    printf("constraints:\n");
    for (NotNull<const Constraint> c : cs->unsolvedConstraints)
    {
        auto it = cs->blockedConstraints.find(c);
        int blockCount = it == cs->blockedConstraints.end() ? 0 : int(it->second);
        printf("\t%d\t%s\n", blockCount, toString(*c, opts).c_str());
    }
}

struct InstantiationQueuer : TypeOnceVisitor
{
    ConstraintSolver* solver;
    NotNull<Scope> scope;
    Location location;

    explicit InstantiationQueuer(NotNull<Scope> scope, const Location& location, ConstraintSolver* solver)
        : solver(solver)
        , scope(scope)
        , location(location)
    {
    }

    bool visit(TypeId ty, const PendingExpansionType& petv) override
    {
        solver->pushConstraint(scope, location, TypeAliasExpansionConstraint{ty});
        return false;
    }

    bool visit(TypeId ty, const TypeFamilyInstanceType&) override
    {
        solver->pushConstraint(scope, location, ReduceConstraint{ty});
        return true;
    }

    bool visit(TypeId ty, const ClassType& ctv) override
    {
        return false;
    }
};

ConstraintSolver::ConstraintSolver(NotNull<Normalizer> normalizer, NotNull<Scope> rootScope, std::vector<NotNull<Constraint>> constraints,
    ModuleName moduleName, NotNull<ModuleResolver> moduleResolver, std::vector<RequireCycle> requireCycles, DcrLogger* logger, TypeCheckLimits limits)
    : arena(normalizer->arena)
    , builtinTypes(normalizer->builtinTypes)
    , normalizer(normalizer)
    , constraints(std::move(constraints))
    , rootScope(rootScope)
    , currentModuleName(std::move(moduleName))
    , moduleResolver(moduleResolver)
    , requireCycles(requireCycles)
    , logger(logger)
    , limits(std::move(limits))
{
    opts.exhaustive = true;

    for (NotNull<Constraint> c : this->constraints)
    {
        unsolvedConstraints.push_back(c);

        // initialize the reference counts for the free types in this constraint.
        for (auto ty : c->getFreeTypes())
        {
            // increment the reference count for `ty`
            auto [refCount, _] = unresolvedConstraints.try_insert(ty, 0);
            refCount += 1;
        }

        for (NotNull<const Constraint> dep : c->dependencies)
        {
            block(dep, c);
        }
    }
}

void ConstraintSolver::randomize(unsigned seed)
{
    if (unsolvedConstraints.empty())
        return;

    unsigned int rng = seed;

    for (size_t i = unsolvedConstraints.size() - 1; i > 0; --i)
    {
        // Fisher-Yates shuffle
        size_t j = rng % (i + 1);

        std::swap(unsolvedConstraints[i], unsolvedConstraints[j]);

        // LCG RNG, constants from Numerical Recipes
        // This may occasionally result in skewed shuffles due to distribution properties, but this is a debugging tool so it should be good enough
        rng = rng * 1664525 + 1013904223;
    }
}

void ConstraintSolver::run()
{
    if (isDone())
        return;

    if (FFlag::DebugLuauLogSolver)
    {
        printf(
            "Starting solver for module %s (%s)\n", moduleResolver->getHumanReadableModuleName(currentModuleName).c_str(), currentModuleName.c_str());
        dump(this, opts);
        printf("Bindings:\n");
        dumpBindings(rootScope, opts);
    }

    if (logger)
    {
        logger->captureInitialSolverState(rootScope, unsolvedConstraints);
    }

    auto runSolverPass = [&](bool force) {
        bool progress = false;

        size_t i = 0;
        while (i < unsolvedConstraints.size())
        {
            NotNull<const Constraint> c = unsolvedConstraints[i];
            if (!force && isBlocked(c))
            {
                ++i;
                continue;
            }

            if (limits.finishTime && TimeTrace::getClock() > *limits.finishTime)
                throwTimeLimitError();
            if (limits.cancellationToken && limits.cancellationToken->requested())
                throwUserCancelError();

            std::string saveMe = FFlag::DebugLuauLogSolver ? toString(*c, opts) : std::string{};
            StepSnapshot snapshot;

            if (logger)
            {
                snapshot = logger->prepareStepSnapshot(rootScope, c, force, unsolvedConstraints);
            }

            bool success = tryDispatch(c, force);

            progress |= success;

            if (success)
            {
                unblock(c);
                unsolvedConstraints.erase(unsolvedConstraints.begin() + i);

                // decrement the referenced free types for this constraint if we dispatched successfully!
                for (auto ty : c->getFreeTypes())
                {
                    // this is a little weird, but because we're only counting free types in subtyping constraints,
                    // some constraints (like unpack) might actually produce _more_ references to a free type.
                    size_t& refCount = unresolvedConstraints[ty];
                    if (refCount > 0)
                        refCount -= 1;
                }

                if (logger)
                {
                    logger->commitStepSnapshot(snapshot);
                }

                if (FFlag::DebugLuauLogSolver)
                {
                    if (force)
                        printf("Force ");
                    printf("Dispatched\n\t%s\n", saveMe.c_str());

                    if (force)
                    {
                        printf("Blocked on:\n");

                        for (const auto& [bci, cv] : blocked)
                        {
                            if (end(cv) == std::find(begin(cv), end(cv), c))
                                continue;

                            if (auto bty = get_if<TypeId>(&bci))
                                printf("\tType %s\n", toString(*bty, opts).c_str());
                            else if (auto btp = get_if<TypePackId>(&bci))
                                printf("\tPack %s\n", toString(*btp, opts).c_str());
                            else if (auto cc = get_if<const Constraint*>(&bci))
                                printf("\tCons %s\n", toString(**cc, opts).c_str());
                            else
                                LUAU_ASSERT(!"Unreachable??");
                        }
                    }

                    dump(this, opts);
                }
            }
            else
                ++i;

            if (force && success)
                return true;
        }

        return progress;
    };

    bool progress = false;
    do
    {
        progress = runSolverPass(false);
        if (!progress)
            progress |= runSolverPass(true);
    } while (progress);

    if (FFlag::DebugLuauLogSolver || FFlag::DebugLuauLogBindings)
        dumpBindings(rootScope, opts);

    if (logger)
    {
        logger->captureFinalSolverState(rootScope, unsolvedConstraints);
    }
}

bool ConstraintSolver::isDone()
{
    return unsolvedConstraints.empty();
}

namespace
{

struct TypeAndLocation
{
    TypeId typeId;
    Location location;
};

} // namespace

bool ConstraintSolver::tryDispatch(NotNull<const Constraint> constraint, bool force)
{
    if (!force && isBlocked(constraint))
        return false;

    bool success = false;

    if (auto sc = get<SubtypeConstraint>(*constraint))
        success = tryDispatch(*sc, constraint, force);
    else if (auto psc = get<PackSubtypeConstraint>(*constraint))
        success = tryDispatch(*psc, constraint, force);
    else if (auto gc = get<GeneralizationConstraint>(*constraint))
        success = tryDispatch(*gc, constraint, force);
    else if (auto ic = get<IterableConstraint>(*constraint))
        success = tryDispatch(*ic, constraint, force);
    else if (auto nc = get<NameConstraint>(*constraint))
        success = tryDispatch(*nc, constraint);
    else if (auto taec = get<TypeAliasExpansionConstraint>(*constraint))
        success = tryDispatch(*taec, constraint);
    else if (auto fcc = get<FunctionCallConstraint>(*constraint))
        success = tryDispatch(*fcc, constraint);
    else if (auto fcc = get<FunctionCheckConstraint>(*constraint))
        success = tryDispatch(*fcc, constraint);
    else if (auto fcc = get<PrimitiveTypeConstraint>(*constraint))
        success = tryDispatch(*fcc, constraint);
    else if (auto hpc = get<HasPropConstraint>(*constraint))
        success = tryDispatch(*hpc, constraint);
    else if (auto spc = get<SetPropConstraint>(*constraint))
        success = tryDispatch(*spc, constraint);
    else if (auto spc = get<HasIndexerConstraint>(*constraint))
        success = tryDispatch(*spc, constraint);
    else if (auto spc = get<SetIndexerConstraint>(*constraint))
        success = tryDispatch(*spc, constraint, force);
    else if (auto uc = get<UnpackConstraint>(*constraint))
        success = tryDispatch(*uc, constraint);
    else if (auto uc = get<Unpack1Constraint>(*constraint))
        success = tryDispatch(*uc, constraint);
    else if (auto rc = get<ReduceConstraint>(*constraint))
        success = tryDispatch(*rc, constraint, force);
    else if (auto rpc = get<ReducePackConstraint>(*constraint))
        success = tryDispatch(*rpc, constraint, force);
    else if (auto eqc = get<EqualityConstraint>(*constraint))
        success = tryDispatch(*eqc, constraint, force);
    else
        LUAU_ASSERT(false);

    if (success)
        unblock(constraint);

    return success;
}

bool ConstraintSolver::tryDispatch(const SubtypeConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    if (isBlocked(c.subType))
        return block(c.subType, constraint);
    else if (isBlocked(c.superType))
        return block(c.superType, constraint);

    unify(constraint, c.subType, c.superType);

    return true;
}

bool ConstraintSolver::tryDispatch(const PackSubtypeConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    if (isBlocked(c.subPack))
        return block(c.subPack, constraint);
    else if (isBlocked(c.superPack))
        return block(c.superPack, constraint);

    unify(constraint, c.subPack, c.superPack);

    return true;
}

bool ConstraintSolver::tryDispatch(const GeneralizationConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    TypeId generalizedType = follow(c.generalizedType);

    if (isBlocked(c.sourceType))
        return block(c.sourceType, constraint);
    else if (get<PendingExpansionType>(generalizedType))
        return block(generalizedType, constraint);

    std::optional<QuantifierResult> generalized;

    Unifier2 u2{NotNull{arena}, builtinTypes, constraint->scope, NotNull{&iceReporter}};

    std::optional<TypeId> generalizedTy = u2.generalize(c.sourceType);
    if (generalizedTy)
        generalized = QuantifierResult{*generalizedTy}; // FIXME insertedGenerics and insertedGenericPacks
    else
        reportError(CodeTooComplex{}, constraint->location);

    if (generalized)
    {
        if (get<BlockedType>(generalizedType))
            bindBlockedType(generalizedType, generalized->result, c.sourceType, constraint);
        else
            unify(constraint, generalizedType, generalized->result);

        for (auto [free, gen] : generalized->insertedGenerics.pairings)
            unify(constraint, free, gen);

        for (auto [free, gen] : generalized->insertedGenericPacks.pairings)
            unify(constraint, free, gen);
    }
    else
    {
        reportError(CodeTooComplex{}, constraint->location);
        asMutable(c.generalizedType)->ty.emplace<BoundType>(builtinTypes->errorRecoveryType());
    }

    unblock(c.generalizedType, constraint->location);
    unblock(c.sourceType, constraint->location);

    for (TypeId ty : c.interiorTypes)
    {
        u2.generalize(ty);
        unblock(ty, constraint->location);
    }

    return true;
}

bool ConstraintSolver::tryDispatch(const IterableConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    /*
     * for .. in loops can play out in a bunch of different ways depending on
     * the shape of iteratee.
     *
     * iteratee might be:
     *  * (nextFn)
     *  * (nextFn, table)
     *  * (nextFn, table, firstIndex)
     *  * table with a metatable and __index
     *  * table with a metatable and __call but no __index (if the metatable has
     *    both, __index takes precedence)
     *  * table with an indexer but no __index or __call (or no metatable)
     *
     * To dispatch this constraint, we need first to know enough about iteratee
     * to figure out which of the above shapes we are actually working with.
     *
     * If `force` is true and we still do not know, we must flag a warning. Type
     * families are the fix for this.
     *
     * Since we need to know all of this stuff about the types of the iteratee,
     * we have no choice but for ConstraintSolver to also be the thing that
     * applies constraints to the types of the iterators.
     */

    auto block_ = [&](auto&& t) {
        if (force)
        {
            // If we haven't figured out the type of the iteratee by now,
            // there's nothing we can do.
            return true;
        }

        block(t, constraint);
        return false;
    };

    TypePack iterator = extendTypePack(*arena, builtinTypes, c.iterator, 3);
    if (iterator.head.size() < 3 && iterator.tail && isBlocked(*iterator.tail))
        return block_(*iterator.tail);

    {
        bool blocked = false;
        for (TypeId t : iterator.head)
        {
            if (isBlocked(t))
            {
                block(t, constraint);
                blocked = true;
            }
        }

        if (blocked)
            return false;
    }

    if (0 == iterator.head.size())
    {
        unify(constraint, builtinTypes->anyTypePack, c.variables);

        return true;
    }

    TypeId nextTy = follow(iterator.head[0]);
    if (get<FreeType>(nextTy))
        return block_(nextTy);

    if (get<FunctionType>(nextTy))
    {
        TypeId tableTy = builtinTypes->nilType;
        if (iterator.head.size() >= 2)
            tableTy = iterator.head[1];

        TypeId firstIndexTy = builtinTypes->nilType;
        if (iterator.head.size() >= 3)
            firstIndexTy = iterator.head[2];

        return tryDispatchIterableFunction(nextTy, tableTy, firstIndexTy, c, constraint, force);
    }

    else
        return tryDispatchIterableTable(iterator.head[0], c, constraint, force);

    return true;
}

bool ConstraintSolver::tryDispatch(const NameConstraint& c, NotNull<const Constraint> constraint)
{
    if (isBlocked(c.namedType))
        return block(c.namedType, constraint);

    TypeId target = follow(c.namedType);

    if (target->persistent || target->owningArena != arena)
        return true;

    if (TableType* ttv = getMutable<TableType>(target))
    {
        if (c.synthetic && !ttv->name)
            ttv->syntheticName = c.name;
        else
        {
            ttv->name = c.name;
            ttv->instantiatedTypeParams = c.typeParameters;
            ttv->instantiatedTypePackParams = c.typePackParameters;
        }
    }
    else if (MetatableType* mtv = getMutable<MetatableType>(target))
        mtv->syntheticName = c.name;
    else if (get<IntersectionType>(target) || get<UnionType>(target))
    {
        // nothing (yet)
    }
    else
        return block(c.namedType, constraint);

    return true;
}

struct InfiniteTypeFinder : TypeOnceVisitor
{
    ConstraintSolver* solver;
    const InstantiationSignature& signature;
    NotNull<Scope> scope;
    bool foundInfiniteType = false;

    explicit InfiniteTypeFinder(ConstraintSolver* solver, const InstantiationSignature& signature, NotNull<Scope> scope)
        : solver(solver)
        , signature(signature)
        , scope(scope)
    {
    }

    bool visit(TypeId ty, const PendingExpansionType& petv) override
    {
        std::optional<TypeFun> tf =
            (petv.prefix) ? scope->lookupImportedType(petv.prefix->value, petv.name.value) : scope->lookupType(petv.name.value);

        if (!tf.has_value())
            return true;

        auto [typeArguments, packArguments] = saturateArguments(solver->arena, solver->builtinTypes, *tf, petv.typeArguments, petv.packArguments);

        if (follow(tf->type) == follow(signature.fn.type) && (signature.arguments != typeArguments || signature.packArguments != packArguments))
        {
            foundInfiniteType = true;
            return false;
        }

        return true;
    }
};

bool ConstraintSolver::tryDispatch(const TypeAliasExpansionConstraint& c, NotNull<const Constraint> constraint)
{
    const PendingExpansionType* petv = get<PendingExpansionType>(follow(c.target));
    if (!petv)
    {
        unblock(c.target, constraint->location);
        return true;
    }

    auto bindResult = [this, &c, constraint](TypeId result) {
        LUAU_ASSERT(get<PendingExpansionType>(c.target));
        asMutable(c.target)->ty.emplace<BoundType>(result);
        unblock(c.target, constraint->location);
    };

    std::optional<TypeFun> tf = (petv->prefix) ? constraint->scope->lookupImportedType(petv->prefix->value, petv->name.value)
                                               : constraint->scope->lookupType(petv->name.value);

    if (!tf.has_value())
    {
        reportError(UnknownSymbol{petv->name.value, UnknownSymbol::Context::Type}, constraint->location);
        bindResult(errorRecoveryType());
        return true;
    }

    // If there are no parameters to the type function we can just use the type
    // directly.
    if (tf->typeParams.empty() && tf->typePackParams.empty())
    {
        bindResult(tf->type);
        return true;
    }

    // Due to how pending expansion types and TypeFun's are created
    // If this check passes, we have created a cyclic / corecursive type alias
    // of size 0
    TypeId lhs = c.target;
    TypeId rhs = tf->type;
    if (occursCheck(lhs, rhs))
    {
        reportError(OccursCheckFailed{}, constraint->location);
        bindResult(errorRecoveryType());
        return true;
    }

    auto [typeArguments, packArguments] = saturateArguments(arena, builtinTypes, *tf, petv->typeArguments, petv->packArguments);

    bool sameTypes = std::equal(typeArguments.begin(), typeArguments.end(), tf->typeParams.begin(), tf->typeParams.end(), [](auto&& itp, auto&& p) {
        return itp == p.ty;
    });

    bool samePacks =
        std::equal(packArguments.begin(), packArguments.end(), tf->typePackParams.begin(), tf->typePackParams.end(), [](auto&& itp, auto&& p) {
            return itp == p.tp;
        });

    // If we're instantiating the type with its generic saturatedTypeArguments we are
    // performing the identity substitution. We can just short-circuit and bind
    // to the TypeFun's type.
    if (sameTypes && samePacks)
    {
        bindResult(tf->type);
        return true;
    }

    InstantiationSignature signature{
        *tf,
        typeArguments,
        packArguments,
    };

    // If we use the same signature, we don't need to bother trying to
    // instantiate the alias again, since the instantiation should be
    // deterministic.
    if (TypeId* cached = instantiatedAliases.find(signature))
    {
        bindResult(*cached);
        return true;
    }

    // In order to prevent infinite types from being expanded and causing us to
    // cycle infinitely, we need to scan the type function for cases where we
    // expand the same alias with different type saturatedTypeArguments. See
    // https://github.com/luau-lang/luau/pull/68 for the RFC responsible for
    // this. This is a little nicer than using a recursion limit because we can
    // catch the infinite expansion before actually trying to expand it.
    InfiniteTypeFinder itf{this, signature, constraint->scope};
    itf.traverse(tf->type);

    if (itf.foundInfiniteType)
    {
        // TODO (CLI-56761): Report an error.
        bindResult(errorRecoveryType());
        reportError(GenericError{"Recursive type being used with different parameters"}, constraint->location);
        return true;
    }

    ApplyTypeFunction applyTypeFunction{arena};
    for (size_t i = 0; i < typeArguments.size(); ++i)
    {
        applyTypeFunction.typeArguments[tf->typeParams[i].ty] = typeArguments[i];
    }

    for (size_t i = 0; i < packArguments.size(); ++i)
    {
        applyTypeFunction.typePackArguments[tf->typePackParams[i].tp] = packArguments[i];
    }

    std::optional<TypeId> maybeInstantiated = applyTypeFunction.substitute(tf->type);
    // Note that ApplyTypeFunction::encounteredForwardedType is never set in
    // DCR, because we do not use free types for forward-declared generic
    // aliases.

    if (!maybeInstantiated.has_value())
    {
        // TODO (CLI-56761): Report an error.
        bindResult(errorRecoveryType());
        return true;
    }

    TypeId instantiated = *maybeInstantiated;
    TypeId target = follow(instantiated);

    // The application is not recursive, so we need to queue up application of
    // any child type function instantiations within the result in order for it
    // to be complete.
    InstantiationQueuer queuer{constraint->scope, constraint->location, this};
    queuer.traverse(target);

    if (target->persistent || target->owningArena != arena)
    {
        bindResult(target);
        return true;
    }

    // Type function application will happily give us the exact same type if
    // there are e.g. generic saturatedTypeArguments that go unused.
    const TableType* tfTable = getTableType(tf->type);
    bool needsClone = follow(tf->type) == target || (tfTable != nullptr && tfTable == getTableType(target));
    // Only tables have the properties we're trying to set.
    TableType* ttv = getMutableTableType(target);

    if (ttv)
    {
        if (needsClone)
        {
            // Substitution::clone is a shallow clone. If this is a
            // metatable type, we want to mutate its table, so we need to
            // explicitly clone that table as well. If we don't, we will
            // mutate another module's type surface and cause a
            // use-after-free.
            if (get<MetatableType>(target))
            {
                instantiated = applyTypeFunction.clone(target);
                MetatableType* mtv = getMutable<MetatableType>(instantiated);
                mtv->table = applyTypeFunction.clone(mtv->table);
                ttv = getMutable<TableType>(mtv->table);
            }
            else if (get<TableType>(target))
            {
                instantiated = applyTypeFunction.clone(target);
                ttv = getMutable<TableType>(instantiated);
            }

            target = follow(instantiated);
        }

        ttv->instantiatedTypeParams = typeArguments;
        ttv->instantiatedTypePackParams = packArguments;
        // TODO: Fill in definitionModuleName.
    }

    bindResult(target);

    instantiatedAliases[signature] = target;

    return true;
}

bool ConstraintSolver::tryDispatch(const FunctionCallConstraint& c, NotNull<const Constraint> constraint)
{
    TypeId fn = follow(c.fn);
    TypePackId argsPack = follow(c.argsPack);
    TypePackId result = follow(c.result);

    if (isBlocked(fn) || hasUnresolvedConstraints(fn))
    {
        return block(c.fn, constraint);
    }

    // if we're calling an error type, the result is an error type, and that's that.
    if (get<ErrorType>(fn))
    {
        asMutable(c.result)->ty.emplace<BoundTypePack>(builtinTypes->errorTypePack);
        unblock(c.result, constraint->location);

        return true;
    }

    if (get<NeverType>(fn))
    {
        asMutable(c.result)->ty.emplace<BoundTypePack>(builtinTypes->neverTypePack);
        unblock(c.result, constraint->location);
        return true;
    }

    auto [argsHead, argsTail] = flatten(argsPack);

    bool blocked = false;
    for (TypeId t : argsHead)
    {
        if (isBlocked(t))
        {
            block(t, constraint);
            blocked = true;
        }
    }

    if (argsTail && isBlocked(*argsTail))
    {
        block(*argsTail, constraint);
        blocked = true;
    }

    if (blocked)
        return false;

    auto collapse = [](const auto* t) -> std::optional<TypeId> {
        auto it = begin(t);
        auto endIt = end(t);

        LUAU_ASSERT(it != endIt);
        TypeId fst = follow(*it);
        while (it != endIt)
        {
            if (follow(*it) != fst)
                return std::nullopt;
            ++it;
        }

        return fst;
    };

    // Sometimes the `fn` type is a union/intersection, but whose constituents are all the same pointer.
    if (auto ut = get<UnionType>(fn))
        fn = collapse(ut).value_or(fn);
    else if (auto it = get<IntersectionType>(fn))
        fn = collapse(it).value_or(fn);

    // We don't support magic __call metamethods.
    if (std::optional<TypeId> callMm = findMetatableEntry(builtinTypes, errors, fn, "__call", constraint->location))
    {
        argsHead.insert(argsHead.begin(), fn);

        if (argsTail && isBlocked(*argsTail))
            return block(*argsTail, constraint);

        argsPack = arena->addTypePack(TypePack{std::move(argsHead), argsTail});
        fn = follow(*callMm);
        asMutable(c.result)->ty.emplace<FreeTypePack>(constraint->scope);
    }
    else
    {
        const FunctionType* ftv = get<FunctionType>(fn);
        bool usedMagic = false;

        if (ftv)
        {
            if (ftv->dcrMagicFunction)
                usedMagic = ftv->dcrMagicFunction(MagicFunctionCallContext{NotNull{this}, constraint, c.callSite, c.argsPack, result});

            if (ftv->dcrMagicRefinement)
                ftv->dcrMagicRefinement(MagicRefinementContext{constraint->scope, c.callSite, c.discriminantTypes});
        }

        if (!usedMagic)
            asMutable(c.result)->ty.emplace<FreeTypePack>(constraint->scope);
    }

    for (std::optional<TypeId> ty : c.discriminantTypes)
    {
        if (!ty || !isBlocked(*ty))
            continue;

        // We use `any` here because the discriminant type may be pointed at by both branches,
        // where the discriminant type is not negated, and the other where it is negated, i.e.
        // `unknown ~ unknown` and `~unknown ~ never`, so `T & unknown ~ T` and `T & ~unknown ~ never`
        // v.s.
        // `any ~ any` and `~any ~ any`, so `T & any ~ T` and `T & ~any ~ T`
        //
        // In practice, users cannot negate `any`, so this is an implementation detail we can always change.
        *asMutable(follow(*ty)) = BoundType{builtinTypes->anyType};
    }

    OverloadResolver resolver{
        builtinTypes, NotNull{arena}, normalizer, constraint->scope, NotNull{&iceReporter}, NotNull{&limits}, constraint->location};
    auto [status, overload] = resolver.selectOverload(fn, argsPack);
    TypeId overloadToUse = fn;
    if (status == OverloadResolver::Analysis::Ok)
        overloadToUse = overload;


    TypeId inferredTy = arena->addType(FunctionType{TypeLevel{}, constraint->scope.get(), argsPack, c.result});
    Unifier2 u2{NotNull{arena}, builtinTypes, constraint->scope, NotNull{&iceReporter}};

    const bool occursCheckPassed = u2.unify(overloadToUse, inferredTy);

    if (!u2.genericSubstitutions.empty() || !u2.genericPackSubstitutions.empty())
    {
        std::optional<TypePackId> subst = instantiate2(arena, std::move(u2.genericSubstitutions), std::move(u2.genericPackSubstitutions), result);
        if (!subst)
        {
            reportError(CodeTooComplex{}, constraint->location);
            result = builtinTypes->errorTypePack;
        }
        else
            result = *subst;

        if (c.result != result)
            asMutable(c.result)->ty.emplace<BoundTypePack>(result);
    }

    for (const auto& [expanded, additions] : u2.expandedFreeTypes)
    {
        for (TypeId addition : additions)
            upperBoundContributors[expanded].push_back(std::make_pair(constraint->location, addition));
    }

    if (occursCheckPassed && c.callSite)
        (*c.astOverloadResolvedTypes)[c.callSite] = inferredTy;

    unblock(c.result, constraint->location);

    InstantiationQueuer queuer{constraint->scope, constraint->location, this};
    queuer.traverse(overloadToUse);
    queuer.traverse(inferredTy);

    return true;
}

bool ConstraintSolver::tryDispatch(const FunctionCheckConstraint& c, NotNull<const Constraint> constraint)
{
    TypeId fn = follow(c.fn);
    const TypePackId argsPack = follow(c.argsPack);

    if (isBlocked(fn))
        return block(fn, constraint);

    if (isBlocked(argsPack))
        return true;

    // We know the type of the function and the arguments it expects to receive.
    // We also know the TypeIds of the actual arguments that will be passed.
    //
    // Bidirectional type checking: Force those TypeIds to be the expected
    // arguments. If something is incoherent, we'll spot it in type checking.
    //
    // Most important detail: If a function argument is a lambda, we also want
    // to force unannotated argument types of that lambda to be the expected
    // types.

    // FIXME: Bidirectional type checking of overloaded functions is not yet supported.
    const FunctionType* ftv = get<FunctionType>(fn);
    if (!ftv)
        return true;

    DenseHashMap<TypeId, TypeId> replacements{nullptr};
    DenseHashMap<TypePackId, TypePackId> replacementPacks{nullptr};

    for (auto generic : ftv->generics)
        replacements[generic] = builtinTypes->unknownType;

    for (auto genericPack : ftv->genericPacks)
        replacementPacks[genericPack] = builtinTypes->unknownTypePack;

    // If the type of the function has generics, we don't actually want to push any of the generics themselves
    // into the argument types as expected types because this creates an unnecessary loop. Instead, we want to
    // replace these types with `unknown` (and `...unknown`) to keep any structure but not create the cycle.
    if (!replacements.empty() || !replacementPacks.empty())
    {
        Replacer replacer{arena, std::move(replacements), std::move(replacementPacks)};

        std::optional<TypeId> res = replacer.substitute(fn);
        if (res)
        {
            if (*res != fn)
            {
                FunctionType* ftvMut = getMutable<FunctionType>(*res);
                LUAU_ASSERT(ftvMut);
                ftvMut->generics.clear();
                ftvMut->genericPacks.clear();
            }

            fn = *res;
            ftv = get<FunctionType>(*res);
            LUAU_ASSERT(ftv);

            // we've potentially copied type families here, so we need to reproduce their reduce constraint.
            reproduceConstraints(constraint->scope, constraint->location, replacer);
        }
    }

    const std::vector<TypeId> expectedArgs = flatten(ftv->argTypes).first;
    const std::vector<TypeId> argPackHead = flatten(argsPack).first;

    // If this is a self call, the types will have more elements than the AST call.
    // We don't attempt to perform bidirectional inference on the self type.
    const size_t typeOffset = c.callSite->self ? 1 : 0;

    for (size_t i = 0; i < c.callSite->args.size && i + typeOffset < expectedArgs.size() && i + typeOffset < argPackHead.size(); ++i)
    {
        const TypeId expectedArgTy = follow(expectedArgs[i + typeOffset]);
        const TypeId actualArgTy = follow(argPackHead[i + typeOffset]);
        const AstExpr* expr = c.callSite->args.data[i];

        (*c.astExpectedTypes)[expr] = expectedArgTy;

        const FunctionType* expectedLambdaTy = get<FunctionType>(expectedArgTy);
        const FunctionType* lambdaTy = get<FunctionType>(actualArgTy);
        const AstExprFunction* lambdaExpr = expr->as<AstExprFunction>();

        if (expectedLambdaTy && lambdaTy && lambdaExpr)
        {
            const std::vector<TypeId> expectedLambdaArgTys = flatten(expectedLambdaTy->argTypes).first;
            const std::vector<TypeId> lambdaArgTys = flatten(lambdaTy->argTypes).first;

            for (size_t j = 0; j < expectedLambdaArgTys.size() && j < lambdaArgTys.size() && j < lambdaExpr->args.size; ++j)
            {
                if (!lambdaExpr->args.data[j]->annotation && get<FreeType>(follow(lambdaArgTys[j])))
                {
                    asMutable(lambdaArgTys[j])->ty.emplace<BoundType>(expectedLambdaArgTys[j]);
                }
            }
        }
        else if (expr->is<AstExprConstantBool>() || expr->is<AstExprConstantString>() || expr->is<AstExprConstantNumber>() ||
                 expr->is<AstExprConstantNil>())
        {
            Unifier2 u2{arena, builtinTypes, constraint->scope, NotNull{&iceReporter}};
            u2.unify(actualArgTy, expectedArgTy);
        }
        else if (expr->is<AstExprTable>())
        {
            Unifier2 u2{arena, builtinTypes, constraint->scope, NotNull{&iceReporter}};
            std::vector<TypeId> toBlock;
            (void)matchLiteralType(c.astTypes, c.astExpectedTypes, builtinTypes, arena, NotNull{&u2}, expectedArgTy, actualArgTy, expr, toBlock);
            for (auto t : toBlock)
                block(t, constraint);
            if (!toBlock.empty())
                return false;
        }
    }

    return true;
}

bool ConstraintSolver::tryDispatch(const PrimitiveTypeConstraint& c, NotNull<const Constraint> constraint)
{
    std::optional<TypeId> expectedType = c.expectedType ? std::make_optional<TypeId>(follow(*c.expectedType)) : std::nullopt;
    if (expectedType && (isBlocked(*expectedType) || get<PendingExpansionType>(*expectedType)))
        return block(*expectedType, constraint);

    const FreeType* freeType = get<FreeType>(follow(c.freeType));

    // if this is no longer a free type, then we're done.
    if (!freeType)
        return true;

    // We will wait if there are any other references to the free type mentioned here.
    // This is probably the only thing that makes this not insane to do.
    if (auto refCount = unresolvedConstraints.find(c.freeType); refCount && *refCount > 1)
    {
        block(c.freeType, constraint);
        return false;
    }

    TypeId bindTo = c.primitiveType;

    if (freeType->upperBound != c.primitiveType && maybeSingleton(freeType->upperBound))
        bindTo = freeType->lowerBound;
    else if (expectedType && maybeSingleton(*expectedType))
        bindTo = freeType->lowerBound;

    asMutable(c.freeType)->ty.emplace<BoundType>(bindTo);

    return true;
}

bool ConstraintSolver::tryDispatch(const HasPropConstraint& c, NotNull<const Constraint> constraint)
{
    const TypeId subjectType = follow(c.subjectType);
    const TypeId resultType = follow(c.resultType);

    LUAU_ASSERT(get<BlockedType>(resultType));
    LUAU_ASSERT(canMutate(resultType, constraint));

    if (isBlocked(subjectType) || get<PendingExpansionType>(subjectType) || get<TypeFamilyInstanceType>(subjectType))
        return block(subjectType, constraint);

    auto [blocked, result] = lookupTableProp(constraint, subjectType, c.prop, c.context, c.inConditional, c.suppressSimplification);
    if (!blocked.empty())
    {
        for (TypeId blocked : blocked)
            block(blocked, constraint);

        return false;
    }

    bindBlockedType(resultType, result.value_or(builtinTypes->anyType), c.subjectType, constraint);
    unblock(resultType, constraint->location);
    return true;
}

static bool isUnsealedTable(TypeId ty)
{
    ty = follow(ty);
    const TableType* ttv = get<TableType>(ty);
    return ttv && ttv->state == TableState::Unsealed;
}

/**
 * Given a path into a set of nested unsealed tables `ty`, insert a new property `replaceTy` as the leaf-most property.
 *
 * Fails and does nothing if every table along the way is not unsealed.
 *
 * Mutates the innermost table type in-place.
 */
static void updateTheTableType(
    NotNull<BuiltinTypes> builtinTypes, NotNull<TypeArena> arena, TypeId ty, const std::vector<std::string>& path, TypeId replaceTy)
{
    if (path.empty())
        return;

    // First walk the path and ensure that it's unsealed tables all the way
    // to the end.
    {
        TypeId t = ty;
        for (size_t i = 0; i < path.size() - 1; ++i)
        {
            if (!isUnsealedTable(t))
                return;

            const TableType* tbl = get<TableType>(t);
            auto it = tbl->props.find(path[i]);
            if (it == tbl->props.end())
                return;

            t = follow(it->second.type());
        }

        // The last path segment should not be a property of the table at all.
        // We are not changing property types.  We are only admitting this one
        // new property to be appended.
        if (!isUnsealedTable(t))
            return;
        const TableType* tbl = get<TableType>(t);
        if (0 != tbl->props.count(path.back()))
            return;
    }

    TypeId t = ty;
    ErrorVec dummy;

    for (size_t i = 0; i < path.size() - 1; ++i)
    {
        t = follow(t);
        auto propTy = findTablePropertyRespectingMeta(builtinTypes, dummy, t, path[i], ValueContext::LValue, Location{});
        dummy.clear();

        if (!propTy)
            return;

        t = *propTy;
    }

    const std::string& lastSegment = path.back();

    t = follow(t);
    TableType* tt = getMutable<TableType>(t);
    if (auto mt = get<MetatableType>(t))
        tt = getMutable<TableType>(mt->table);

    if (!tt)
        return;

    tt->props[lastSegment].setType(replaceTy);
}

bool ConstraintSolver::tryDispatch(const SetPropConstraint& c, NotNull<const Constraint> constraint)
{
    TypeId subjectType = follow(c.subjectType);
    const TypeId propType = follow(c.propType);

    if (isBlocked(subjectType))
        return block(subjectType, constraint);

    std::optional<TypeId> existingPropType = subjectType;

    LUAU_ASSERT(!c.path.empty());
    if (c.path.empty())
        return false;

    for (size_t i = 0; i < c.path.size(); ++i)
    {
        const std::string& segment = c.path[i];
        if (!existingPropType)
            break;

        ValueContext ctx = i == c.path.size() - 1 ? ValueContext::LValue : ValueContext::RValue;

        auto [blocked, result] = lookupTableProp(constraint, *existingPropType, segment, ctx);
        if (!blocked.empty())
        {
            for (TypeId blocked : blocked)
                block(blocked, constraint);
            return false;
        }

        existingPropType = result;
    }

    auto bind = [&](TypeId a, TypeId b) {
        bindBlockedType(a, b, subjectType, constraint);
    };

    if (existingPropType)
    {
        unify(constraint, propType, *existingPropType);
        unify(constraint, *existingPropType, propType);
        bind(c.resultType, c.subjectType);
        unblock(c.resultType, constraint->location);
        return true;
    }

    const TypeId originalSubjectType = subjectType;

    if (auto mt = get<MetatableType>(subjectType))
        subjectType = follow(mt->table);

    if (get<FreeType>(subjectType))
        return false;
    else if (auto ttv = getMutable<TableType>(subjectType))
    {
        if (ttv->state == TableState::Free)
        {
            LUAU_ASSERT(!subjectType->persistent);

            ttv->props[c.path[0]] = Property{propType};
            bind(c.resultType, subjectType);
            unblock(c.resultType, constraint->location);
            return true;
        }
        else if (ttv->state == TableState::Unsealed)
        {
            LUAU_ASSERT(!subjectType->persistent);

            updateTheTableType(builtinTypes, NotNull{arena}, subjectType, c.path, propType);
        }
    }

    bind(c.resultType, originalSubjectType);
    unblock(c.resultType, constraint->location);
    return true;
}

bool ConstraintSolver::tryDispatchHasIndexer(
    int& recursionDepth, NotNull<const Constraint> constraint, TypeId subjectType, TypeId indexType, TypeId resultType, Set<TypeId>& seen)
{
    RecursionLimiter _rl{&recursionDepth, FInt::LuauSolverRecursionLimit};

    subjectType = follow(subjectType);
    indexType = follow(indexType);

    if (seen.contains(subjectType))
        return false;
    seen.insert(subjectType);

    LUAU_ASSERT(get<BlockedType>(resultType));
    LUAU_ASSERT(canMutate(resultType, constraint));

    if (auto ft = get<FreeType>(subjectType))
    {
        FreeType freeResult{ft->scope, builtinTypes->neverType, builtinTypes->unknownType};
        asMutable(resultType)->ty.emplace<FreeType>(freeResult);

        TypeId upperBound = arena->addType(TableType{/* props */ {}, TableIndexer{indexType, resultType}, TypeLevel{}, TableState::Unsealed});

        unify(constraint, subjectType, upperBound);

        return true;
    }
    else if (auto tt = getMutable<TableType>(subjectType))
    {
        if (auto indexer = tt->indexer)
        {
            unify(constraint, indexType, indexer->indexType);

            bindBlockedType(resultType, indexer->indexResultType, subjectType, constraint);
            return true;
        }
        else if (tt->state == TableState::Unsealed)
        {
            // FIXME this is greedy.

            FreeType freeResult{tt->scope, builtinTypes->neverType, builtinTypes->unknownType};
            asMutable(resultType)->ty.emplace<FreeType>(freeResult);

            tt->indexer = TableIndexer{indexType, resultType};
            return true;
        }
    }
    else if (auto mt = get<MetatableType>(subjectType))
        return tryDispatchHasIndexer(recursionDepth, constraint, mt->table, indexType, resultType, seen);
    else if (auto ct = get<ClassType>(subjectType))
    {
        if (auto indexer = ct->indexer)
        {
            unify(constraint, indexType, indexer->indexType);
            bindBlockedType(resultType, indexer->indexResultType, subjectType, constraint);
            return true;
        }
        else if (isString(indexType))
        {
            bindBlockedType(resultType, builtinTypes->unknownType, subjectType, constraint);
            return true;
        }
    }
    else if (auto it = get<IntersectionType>(subjectType))
    {
        // subjectType <: {[indexType]: resultType}
        //
        // 'a & ~(false | nil) <: {[indexType]: resultType}
        //
        // 'a <: {[indexType]: resultType}
        // ~(false | nil) <: {[indexType]: resultType}

        Set<TypeId> parts{nullptr};
        for (TypeId part : it)
            parts.insert(follow(part));

        Set<TypeId> results{nullptr};

        for (TypeId part : parts)
        {
            TypeId r = arena->addType(BlockedType{});
            getMutable<BlockedType>(r)->setOwner(const_cast<Constraint*>(constraint.get()));

            bool ok = tryDispatchHasIndexer(recursionDepth, constraint, part, indexType, r, seen);
            // If we've cut a recursive loop short, skip it.
            if (!ok)
                continue;

            r = follow(r);
            if (!get<ErrorType>(r))
                results.insert(r);
        }

        if (0 == results.size())
            bindBlockedType(resultType, builtinTypes->errorType, subjectType, constraint);
        else if (1 == results.size())
            bindBlockedType(resultType, *results.begin(), subjectType, constraint);
        else
            asMutable(resultType)->ty.emplace<IntersectionType>(std::vector(results.begin(), results.end()));

        return true;
    }
    else if (auto ut = get<UnionType>(subjectType))
    {
        Set<TypeId> parts{nullptr};
        for (TypeId part : ut)
            parts.insert(follow(part));

        Set<TypeId> results{nullptr};

        for (TypeId part : parts)
        {
            TypeId r = arena->addType(BlockedType{});
            getMutable<BlockedType>(r)->setOwner(const_cast<Constraint*>(constraint.get()));

            bool ok = tryDispatchHasIndexer(recursionDepth, constraint, part, indexType, r, seen);
            // If we've cut a recursive loop short, skip it.
            if (!ok)
                continue;

            r = follow(r);
            if (!get<ErrorType>(r))
                results.insert(r);
        }

        if (0 == results.size())
            asMutable(resultType)->ty.emplace<BoundType>(builtinTypes->errorType);
        else if (1 == results.size())
            asMutable(resultType)->ty.emplace<BoundType>(*results.begin());
        else
            asMutable(resultType)->ty.emplace<UnionType>(std::vector(results.begin(), results.end()));

        return true;
    }

    bindBlockedType(resultType, builtinTypes->errorType, subjectType, constraint);

    return true;
}

namespace
{

struct BlockedTypeFinder : TypeOnceVisitor
{
    std::optional<TypeId> blocked;

    bool visit(TypeId ty) override
    {
        // If we've already found one, stop traversing.
        return !blocked.has_value();
    }

    bool visit(TypeId ty, const BlockedType&) override
    {
        blocked = ty;
        return false;
    }
};

} // namespace

bool ConstraintSolver::tryDispatch(const HasIndexerConstraint& c, NotNull<const Constraint> constraint)
{
    const TypeId subjectType = follow(c.subjectType);
    const TypeId indexType = follow(c.indexType);

    if (isBlocked(subjectType))
        return block(subjectType, constraint);

    if (isBlocked(indexType))
        return block(indexType, constraint);

    BlockedTypeFinder btf;

    btf.visit(subjectType);

    if (btf.blocked)
        return block(*btf.blocked, constraint);
    int recursionDepth = 0;

    Set<TypeId> seen{nullptr};

    return tryDispatchHasIndexer(recursionDepth, constraint, subjectType, indexType, c.resultType, seen);
}

std::pair<bool, std::optional<TypeId>> ConstraintSolver::tryDispatchSetIndexer(
    NotNull<const Constraint> constraint, TypeId subjectType, TypeId indexType, TypeId propType, bool expandFreeTypeBounds)
{
    if (isBlocked(subjectType))
        return {block(subjectType, constraint), std::nullopt};

    if (auto tt = getMutable<TableType>(subjectType))
    {
        if (tt->indexer)
        {
            unify(constraint, indexType, tt->indexer->indexType);
            return {true, tt->indexer->indexResultType};
        }
        else if (tt->state == TableState::Free || tt->state == TableState::Unsealed)
        {
            TypeId resultTy = freshType(arena, builtinTypes, constraint->scope.get());
            tt->indexer = TableIndexer{indexType, resultTy};
            return {true, resultTy};
        }
    }
    else if (auto ft = getMutable<FreeType>(subjectType); ft && expandFreeTypeBounds)
    {
        // Setting an indexer on some fresh type means we use that fresh type in a negative position.
        // Therefore, we only care about the upper bound.
        //
        // We'll extend the upper bound if we could dispatch, but could not find a table type to update the indexer.
        auto [dispatched, resultTy] = tryDispatchSetIndexer(constraint, ft->upperBound, indexType, propType, /*expandFreeTypeBounds=*/false);
        if (dispatched && !resultTy)
        {
            // Despite that we haven't found a table type, adding a table type causes us to have one that we can /now/ find.
            resultTy = freshType(arena, builtinTypes, constraint->scope.get());

            TypeId tableTy = arena->addType(TableType{TableState::Sealed, TypeLevel{}, constraint->scope.get()});
            TableType* tt2 = getMutable<TableType>(tableTy);
            tt2->indexer = TableIndexer{indexType, *resultTy};

            ft->upperBound =
                simplifyIntersection(builtinTypes, arena, ft->upperBound, tableTy).result; // TODO: intersect type family or a constraint.
        }

        return {dispatched, resultTy};
    }
    else if (auto it = get<IntersectionType>(subjectType))
    {
        bool dispatched = true;
        std::vector<TypeId> results;

        for (TypeId part : it)
        {
            auto [dispatched2, found] = tryDispatchSetIndexer(constraint, part, indexType, propType, expandFreeTypeBounds);
            dispatched &= dispatched2;
            results.push_back(found.value_or(builtinTypes->errorRecoveryType()));

            if (!dispatched)
                return {dispatched, std::nullopt};
        }

        TypeId resultTy = arena->addType(TypeFamilyInstanceType{
            NotNull{&kBuiltinTypeFamilies.unionFamily},
            std::move(results),
            {},
        });

        pushConstraint(constraint->scope, constraint->location, ReduceConstraint{resultTy});

        return {dispatched, resultTy};
    }
    else if (is<AnyType, ErrorType, NeverType>(subjectType))
        return {true, subjectType};

    return {true, std::nullopt};
}

bool ConstraintSolver::tryDispatch(const SetIndexerConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    TypeId subjectType = follow(c.subjectType);
    if (isBlocked(subjectType))
        return block(subjectType, constraint);

    auto [dispatched, resultTy] = tryDispatchSetIndexer(constraint, subjectType, c.indexType, c.propType, /*expandFreeTypeBounds=*/true);
    if (dispatched)
    {
        bindBlockedType(c.propType, resultTy.value_or(builtinTypes->errorRecoveryType()), subjectType, constraint);
        unblock(c.propType, constraint->location);
    }

    return dispatched;
}

bool ConstraintSolver::tryDispatchUnpack1(NotNull<const Constraint> constraint, TypeId resultTy, TypeId srcTy, bool resultIsLValue)
{
    resultTy = follow(resultTy);
    LUAU_ASSERT(canMutate(resultTy, constraint));

    auto tryExpand = [&](TypeId ty) {
        LocalType* lt = getMutable<LocalType>(ty);
        if (!lt || !resultIsLValue)
            return;

        lt->domain = simplifyUnion(builtinTypes, arena, lt->domain, srcTy).result;
        LUAU_ASSERT(lt->blockCount > 0);
        --lt->blockCount;

        if (0 == lt->blockCount)
            asMutable(ty)->ty.emplace<BoundType>(lt->domain);
    };

    if (auto ut = get<UnionType>(resultTy))
        std::for_each(begin(ut), end(ut), tryExpand);
    else if (get<LocalType>(resultTy))
        tryExpand(resultTy);
    else if (get<BlockedType>(resultTy))
    {
        if (follow(srcTy) == resultTy)
        {
            // It is sometimes the case that we find that a blocked type
            // is only blocked on itself. This doesn't actually
            // constitute any meaningful constraint, so we replace it
            // with a free type.
            TypeId f = freshType(arena, builtinTypes, constraint->scope);
            asMutable(resultTy)->ty.emplace<BoundType>(f);
        }
        else
            bindBlockedType(resultTy, srcTy, srcTy, constraint);
    }
    else
    {
        LUAU_ASSERT(resultIsLValue);
        unify(constraint, srcTy, resultTy);
    }

    unblock(resultTy, constraint->location);
    return true;
}

bool ConstraintSolver::tryDispatch(const UnpackConstraint& c, NotNull<const Constraint> constraint)
{
    TypePackId sourcePack = follow(c.sourcePack);
    TypePackId resultPack = follow(c.resultPack);

    if (isBlocked(sourcePack))
        return block(sourcePack, constraint);

    if (isBlocked(resultPack))
    {
        LUAU_ASSERT(canMutate(resultPack, constraint));
        LUAU_ASSERT(resultPack != sourcePack);
        asMutable(resultPack)->ty.emplace<BoundTypePack>(sourcePack);
        unblock(resultPack, constraint->location);
        return true;
    }

    TypePack srcPack = extendTypePack(*arena, builtinTypes, sourcePack, size(resultPack));

    auto resultIter = begin(resultPack);
    auto resultEnd = end(resultPack);

    size_t i = 0;
    while (resultIter != resultEnd)
    {
        if (i >= srcPack.head.size())
            break;

        TypeId srcTy = follow(srcPack.head[i]);
        TypeId resultTy = follow(*resultIter);
        tryDispatchUnpack1(constraint, resultTy, srcTy, c.resultIsLValue);

        ++resultIter;
        ++i;
    }

    // We know that resultPack does not have a tail, but we don't know if
    // sourcePack is long enough to fill every value.  Replace every remaining
    // result TypeId with `nil`.

    while (resultIter != resultEnd)
    {
        TypeId resultTy = follow(*resultIter);
        LUAU_ASSERT(canMutate(resultTy, constraint));
        if (auto lt = getMutable<LocalType>(resultTy); c.resultIsLValue && lt)
        {
            lt->domain = simplifyUnion(builtinTypes, arena, lt->domain, builtinTypes->nilType).result;
            LUAU_ASSERT(0 <= lt->blockCount);
            --lt->blockCount;

            if (0 == lt->blockCount)
                asMutable(resultTy)->ty.emplace<BoundType>(lt->domain);
        }
        else if (get<BlockedType>(resultTy) || get<PendingExpansionType>(resultTy))
        {
            asMutable(resultTy)->ty.emplace<BoundType>(builtinTypes->nilType);
            unblock(resultTy, constraint->location);
        }

        ++resultIter;
    }

    return true;
}

bool ConstraintSolver::tryDispatch(const Unpack1Constraint& c, NotNull<const Constraint> constraint)
{
    return tryDispatchUnpack1(constraint, c.resultType, c.sourceType, c.resultIsLValue);
}

bool ConstraintSolver::tryDispatch(const ReduceConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    TypeId ty = follow(c.ty);
    FamilyGraphReductionResult result =
        reduceFamilies(ty, constraint->location, TypeFamilyContext{NotNull{this}, constraint->scope, constraint}, force);

    for (TypeId r : result.reducedTypes)
        unblock(r, constraint->location);

    for (TypePackId r : result.reducedPacks)
        unblock(r, constraint->location);

    bool reductionFinished = result.blockedTypes.empty() && result.blockedPacks.empty();

    if (force || reductionFinished)
    {
        // if we're completely dispatching this constraint, we want to record any uninhabited type families to unblock.
        for (auto error : result.errors)
        {
            if (auto utf = get<UninhabitedTypeFamily>(error))
                uninhabitedTypeFamilies.insert(utf->ty);
            else if (auto utpf = get<UninhabitedTypePackFamily>(error))
                uninhabitedTypeFamilies.insert(utpf->tp);
        }
    }

    if (force)
        return true;

    for (TypeId b : result.blockedTypes)
        block(b, constraint);

    for (TypePackId b : result.blockedPacks)
        block(b, constraint);

    return reductionFinished;
}

bool ConstraintSolver::tryDispatch(const ReducePackConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    TypePackId tp = follow(c.tp);
    FamilyGraphReductionResult result =
        reduceFamilies(tp, constraint->location, TypeFamilyContext{NotNull{this}, constraint->scope, constraint}, force);

    for (TypeId r : result.reducedTypes)
        unblock(r, constraint->location);

    for (TypePackId r : result.reducedPacks)
        unblock(r, constraint->location);

    bool reductionFinished = result.blockedTypes.empty() && result.blockedPacks.empty();

    if (force || reductionFinished)
    {
        // if we're completely dispatching this constraint, we want to record any uninhabited type families to unblock.
        for (auto error : result.errors)
        {
            if (auto utf = get<UninhabitedTypeFamily>(error))
                uninhabitedTypeFamilies.insert(utf->ty);
            else if (auto utpf = get<UninhabitedTypePackFamily>(error))
                uninhabitedTypeFamilies.insert(utpf->tp);
        }
    }

    if (force)
        return true;

    for (TypeId b : result.blockedTypes)
        block(b, constraint);

    for (TypePackId b : result.blockedPacks)
        block(b, constraint);

    return reductionFinished;
}

bool ConstraintSolver::tryDispatch(const EqualityConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    unify(constraint, c.resultType, c.assignmentType);
    unify(constraint, c.assignmentType, c.resultType);
    return true;
}

bool ConstraintSolver::tryDispatchIterableTable(TypeId iteratorTy, const IterableConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    auto block_ = [&](auto&& t) {
        if (force)
        {
            // TODO: I believe it is the case that, if we are asked to force
            // this constraint, then we can do nothing but fail.  I'd like to
            // find a code sample that gets here.
            LUAU_ASSERT(false);
        }
        else
            block(t, constraint);
        return false;
    };

    // We may have to block here if we don't know what the iteratee type is,
    // if it's a free table, if we don't know it has a metatable, and so on.
    iteratorTy = follow(iteratorTy);
    if (get<FreeType>(iteratorTy))
        return block_(iteratorTy);

    auto unpack = [&](TypeId ty) {
        TypePackId variadic = arena->addTypePack(VariadicTypePack{ty});
        pushConstraint(constraint->scope, constraint->location, UnpackConstraint{c.variables, variadic, /* resultIsLValue */ true});
    };

    if (get<AnyType>(iteratorTy))
    {
        unpack(builtinTypes->anyType);
        return true;
    }

    if (get<ErrorType>(iteratorTy))
    {
        unpack(builtinTypes->errorType);
        return true;
    }

    if (get<NeverType>(iteratorTy))
    {
        unpack(builtinTypes->neverType);
        return true;
    }

    // Irksome: I don't think we have any way to guarantee that this table
    // type never has a metatable.

    if (auto iteratorTable = get<TableType>(iteratorTy))
    {
        /*
         * We try not to dispatch IterableConstraints over free tables because
         * it's possible that there are other constraints on the table that will
         * clarify what we should do.
         *
         * We should eventually introduce a type family to talk about iteration.
         */
        if (iteratorTable->state == TableState::Free && !force)
            return block(iteratorTy, constraint);

        if (iteratorTable->indexer)
        {
            TypePackId expectedVariablePack = arena->addTypePack({iteratorTable->indexer->indexType, iteratorTable->indexer->indexResultType});
            unify(constraint, c.variables, expectedVariablePack);

            auto [variableTys, variablesTail] = flatten(c.variables);

            // the local types for the indexer _should_ be all set after unification
            for (TypeId ty : variableTys)
            {
                if (auto lt = getMutable<LocalType>(ty))
                {
                    LUAU_ASSERT(lt->blockCount > 0);
                    --lt->blockCount;

                    LUAU_ASSERT(0 <= lt->blockCount);

                    if (0 == lt->blockCount)
                        asMutable(ty)->ty.emplace<BoundType>(lt->domain);
                }
            }
        }
        else
            unpack(builtinTypes->errorType);
    }
    else if (std::optional<TypeId> iterFn = findMetatableEntry(builtinTypes, errors, iteratorTy, "__iter", Location{}))
    {
        if (isBlocked(*iterFn))
        {
            return block(*iterFn, constraint);
        }

        if (std::optional<TypeId> instantiatedIterFn = instantiate(builtinTypes, arena, NotNull{&limits}, constraint->scope, *iterFn))
        {
            if (auto iterFtv = get<FunctionType>(*instantiatedIterFn))
            {
                TypePackId expectedIterArgs = arena->addTypePack({iteratorTy});
                unify(constraint, iterFtv->argTypes, expectedIterArgs);

                TypePack iterRets = extendTypePack(*arena, builtinTypes, iterFtv->retTypes, 2);

                if (iterRets.head.size() < 1)
                {
                    // We've done what we can; this will get reported as an
                    // error by the type checker.
                    return true;
                }

                TypeId nextFn = iterRets.head[0];

                if (std::optional<TypeId> instantiatedNextFn = instantiate(builtinTypes, arena, NotNull{&limits}, constraint->scope, nextFn))
                {
                    const FunctionType* nextFn = get<FunctionType>(*instantiatedNextFn);
                    LUAU_ASSERT(nextFn);
                    const TypePackId nextRetPack = nextFn->retTypes;

                    pushConstraint(constraint->scope, constraint->location, UnpackConstraint{c.variables, nextRetPack, /* resultIsLValue=*/true});
                    return true;
                }
                else
                {
                    reportError(UnificationTooComplex{}, constraint->location);
                }
            }
            else
            {
                // TODO: Support __call and function overloads (what does an overload even mean for this?)
            }
        }
        else
        {
            reportError(UnificationTooComplex{}, constraint->location);
        }
    }
    else if (auto iteratorMetatable = get<MetatableType>(iteratorTy))
    {
        TypeId metaTy = follow(iteratorMetatable->metatable);
        if (get<FreeType>(metaTy))
            return block_(metaTy);

        LUAU_ASSERT(false);
    }
    else if (auto primitiveTy = get<PrimitiveType>(iteratorTy); primitiveTy && primitiveTy->type == PrimitiveType::Type::Table)
        unpack(builtinTypes->unknownType);
    else
        unpack(builtinTypes->errorType);

    return true;
}

bool ConstraintSolver::tryDispatchIterableFunction(
    TypeId nextTy, TypeId tableTy, TypeId firstIndexTy, const IterableConstraint& c, NotNull<const Constraint> constraint, bool force)
{
    // We need to know whether or not this type is nil or not.
    // If we don't know, block and reschedule ourselves.
    firstIndexTy = follow(firstIndexTy);
    if (get<FreeType>(firstIndexTy))
    {
        if (force)
            LUAU_ASSERT(false);
        else
            block(firstIndexTy, constraint);
        return false;
    }

    const FunctionType* nextFn = get<FunctionType>(nextTy);
    // If this does not hold, we should've never called `tryDispatchIterableFunction` in the first place.
    LUAU_ASSERT(nextFn);
    const TypePackId nextRetPack = nextFn->retTypes;

    // the type of the `nextAstFragment` is the `nextTy`.
    (*c.astForInNextTypes)[c.nextAstFragment] = nextTy;

    auto it = begin(nextRetPack);
    std::vector<TypeId> modifiedNextRetHead;

    // The first value is never nil in the context of the loop, even if it's nil
    // in the next function's return type, because the loop will not advance if
    // it's nil.
    if (it != end(nextRetPack))
    {
        TypeId firstRet = *it;
        TypeId modifiedFirstRet = stripNil(builtinTypes, *arena, firstRet);
        modifiedNextRetHead.push_back(modifiedFirstRet);
        ++it;
    }

    for (; it != end(nextRetPack); ++it)
        modifiedNextRetHead.push_back(*it);

    TypePackId modifiedNextRetPack = arena->addTypePack(std::move(modifiedNextRetHead), it.tail());
    auto psc = pushConstraint(constraint->scope, constraint->location, UnpackConstraint{c.variables, modifiedNextRetPack, /* resultIsLValue */ true});
    inheritBlocks(constraint, psc);

    return true;
}

std::pair<std::vector<TypeId>, std::optional<TypeId>> ConstraintSolver::lookupTableProp(NotNull<const Constraint> constraint, TypeId subjectType,
    const std::string& propName, ValueContext context, bool inConditional, bool suppressSimplification)
{
    DenseHashSet<TypeId> seen{nullptr};
    return lookupTableProp(constraint, subjectType, propName, context, inConditional, suppressSimplification, seen);
}

std::pair<std::vector<TypeId>, std::optional<TypeId>> ConstraintSolver::lookupTableProp(NotNull<const Constraint> constraint, TypeId subjectType,
    const std::string& propName, ValueContext context, bool inConditional, bool suppressSimplification, DenseHashSet<TypeId>& seen)
{
    if (seen.contains(subjectType))
        return {};
    seen.insert(subjectType);

    subjectType = follow(subjectType);

    if (isBlocked(subjectType))
        return {{subjectType}, std::nullopt};
    else if (get<AnyType>(subjectType) || get<NeverType>(subjectType))
    {
        return {{}, subjectType};
    }
    else if (auto ttv = getMutable<TableType>(subjectType))
    {
        if (auto prop = ttv->props.find(propName); prop != ttv->props.end())
        {
            switch (context)
            {
            case ValueContext::RValue:
                if (auto rt = prop->second.readTy)
                    return {{}, rt};
                break;
            case ValueContext::LValue:
                if (auto wt = prop->second.writeTy)
                    return {{}, wt};
                break;
            }
        }

        if (ttv->indexer && maybeString(ttv->indexer->indexType))
            return {{}, ttv->indexer->indexResultType};

        if (ttv->state == TableState::Free)
        {
            TypeId result = freshType(arena, builtinTypes, ttv->scope);
            switch (context)
            {
            case ValueContext::RValue:
                ttv->props[propName].readTy = result;
                break;
            case ValueContext::LValue:
                if (auto it = ttv->props.find(propName); it != ttv->props.end() && it->second.isReadOnly())
                {
                    // We do infer read-only properties, but we do not infer
                    // separate read and write types.
                    //
                    // If we encounter a case where a free table has a read-only
                    // property that we subsequently sense a write to, we make
                    // the judgement that the property is read-write and that
                    // both the read and write types are the same.

                    Property& prop = it->second;

                    prop.writeTy = prop.readTy;
                    return {{}, *prop.readTy};
                }
                else
                    ttv->props[propName] = Property::rw(result);

                break;
            }
            return {{}, result};
        }

        // if we are in a conditional context, we treat the property as present and `unknown` because
        // we may be _refining_ a table to include that property. we will want to revisit this a bit
        // in the future once luau has support for exact tables since this only applies when inexact.
        if (inConditional)
            return {{}, builtinTypes->unknownType};
    }
    else if (auto mt = get<MetatableType>(subjectType); mt && context == ValueContext::RValue)
    {
        auto [blocked, result] = lookupTableProp(constraint, mt->table, propName, context, inConditional, suppressSimplification, seen);
        if (!blocked.empty() || result)
            return {blocked, result};

        TypeId mtt = follow(mt->metatable);

        if (get<BlockedType>(mtt))
            return {{mtt}, std::nullopt};
        else if (auto metatable = get<TableType>(mtt))
        {
            auto indexProp = metatable->props.find("__index");
            if (indexProp == metatable->props.end())
                return {{}, result};

            // TODO: __index can be an overloaded function.

            TypeId indexType = follow(indexProp->second.type());

            if (auto ft = get<FunctionType>(indexType))
            {
                TypePack rets = extendTypePack(*arena, builtinTypes, ft->retTypes, 1);
                if (1 == rets.head.size())
                    return {{}, rets.head[0]};
                else
                {
                    // This should probably be an error: We need the first result of the MT.__index method,
                    // but it returns 0 values.  See CLI-68672
                    return {{}, builtinTypes->nilType};
                }
            }
            else
                return lookupTableProp(constraint, indexType, propName, context, inConditional, suppressSimplification, seen);
        }
        else if (get<MetatableType>(mtt))
            return lookupTableProp(constraint, mtt, propName, context, inConditional, suppressSimplification, seen);
    }
    else if (auto ct = get<ClassType>(subjectType))
    {
        if (auto p = lookupClassProp(ct, propName))
            return {{}, context == ValueContext::RValue ? p->readTy : p->writeTy};
        if (ct->indexer)
        {
            return {{}, ct->indexer->indexResultType};
        }
    }
    else if (auto pt = get<PrimitiveType>(subjectType); pt && pt->metatable)
    {
        const TableType* metatable = get<TableType>(follow(*pt->metatable));
        LUAU_ASSERT(metatable);

        auto indexProp = metatable->props.find("__index");
        if (indexProp == metatable->props.end())
            return {{}, std::nullopt};

        return lookupTableProp(constraint, indexProp->second.type(), propName, context, inConditional, suppressSimplification, seen);
    }
    else if (auto ft = get<FreeType>(subjectType))
    {
        const TypeId upperBound = follow(ft->upperBound);

        if (get<TableType>(upperBound) || get<PrimitiveType>(upperBound))
            return lookupTableProp(constraint, upperBound, propName, context, inConditional, suppressSimplification, seen);

        // TODO: The upper bound could be an intersection that contains suitable tables or classes.

        NotNull<Scope> scope{ft->scope};

        const TypeId newUpperBound = arena->addType(TableType{TableState::Free, TypeLevel{}, scope});
        TableType* tt = getMutable<TableType>(newUpperBound);
        LUAU_ASSERT(tt);
        TypeId propType = freshType(arena, builtinTypes, scope);

        switch (context)
        {
        case ValueContext::RValue:
            tt->props[propName] = Property::readonly(propType);
            break;
        case ValueContext::LValue:
            tt->props[propName] = Property::rw(propType);
            break;
        }

        unify(constraint, subjectType, newUpperBound);

        return {{}, propType};
    }
    else if (auto utv = get<UnionType>(subjectType))
    {
        std::vector<TypeId> blocked;
        std::set<TypeId> options;

        for (TypeId ty : utv)
        {
            auto [innerBlocked, innerResult] = lookupTableProp(constraint, ty, propName, context, inConditional, suppressSimplification, seen);
            blocked.insert(blocked.end(), innerBlocked.begin(), innerBlocked.end());
            if (innerResult)
                options.insert(*innerResult);
        }

        if (!blocked.empty())
            return {blocked, std::nullopt};

        if (options.empty())
            return {{}, std::nullopt};
        else if (options.size() == 1)
            return {{}, *begin(options)};
        else if (options.size() == 2 && !suppressSimplification)
        {
            TypeId one = *begin(options);
            TypeId two = *(++begin(options));

            // if we're in an lvalue context, we need the _common_ type here.
            if (context == ValueContext::LValue)
                return {{}, simplifyIntersection(builtinTypes, arena, one, two).result};

            return {{}, simplifyUnion(builtinTypes, arena, one, two).result};
        }
        // if we're in an lvalue context, we need the _common_ type here.
        else if (context == ValueContext::LValue)
            return {{}, arena->addType(IntersectionType{std::vector<TypeId>(begin(options), end(options))})};
        else
            return {{}, arena->addType(UnionType{std::vector<TypeId>(begin(options), end(options))})};
    }
    else if (auto itv = get<IntersectionType>(subjectType))
    {
        std::vector<TypeId> blocked;
        std::set<TypeId> options;

        for (TypeId ty : itv)
        {
            auto [innerBlocked, innerResult] = lookupTableProp(constraint, ty, propName, context, inConditional, suppressSimplification, seen);
            blocked.insert(blocked.end(), innerBlocked.begin(), innerBlocked.end());
            if (innerResult)
                options.insert(*innerResult);
        }

        if (!blocked.empty())
            return {blocked, std::nullopt};

        if (options.empty())
            return {{}, std::nullopt};
        else if (options.size() == 1)
            return {{}, *begin(options)};
        else if (options.size() == 2 && !suppressSimplification)
        {
            TypeId one = *begin(options);
            TypeId two = *(++begin(options));
            return {{}, simplifyIntersection(builtinTypes, arena, one, two).result};
        }
        else
            return {{}, arena->addType(IntersectionType{std::vector<TypeId>(begin(options), end(options))})};
    }
    else if (auto pt = get<PrimitiveType>(subjectType))
    {
        // if we are in a conditional context, we treat the property as present and `unknown` because
        // we may be _refining_ a table to include that property. we will want to revisit this a bit
        // in the future once luau has support for exact tables since this only applies when inexact.
        if (inConditional && pt->type == PrimitiveType::Table)
            return {{}, builtinTypes->unknownType};
    }

    return {{}, std::nullopt};
}

template<typename TID>
bool ConstraintSolver::unify(NotNull<const Constraint> constraint, TID subTy, TID superTy)
{
    Unifier2 u2{NotNull{arena}, builtinTypes, constraint->scope, NotNull{&iceReporter}, &uninhabitedTypeFamilies};

    const bool ok = u2.unify(subTy, superTy);

    for (ConstraintV& c : u2.incompleteSubtypes)
    {
        NotNull<Constraint> addition = pushConstraint(constraint->scope, constraint->location, std::move(c));
        inheritBlocks(constraint, addition);
    }

    if (ok)
    {
        for (const auto& [expanded, additions] : u2.expandedFreeTypes)
        {
            for (TypeId addition : additions)
                upperBoundContributors[expanded].push_back(std::make_pair(constraint->location, addition));
        }
    }
    else
    {
        reportError(OccursCheckFailed{}, constraint->location);
        return false;
    }

    unblock(subTy, constraint->location);
    unblock(superTy, constraint->location);

    return true;
}

void ConstraintSolver::bindBlockedType(TypeId blockedTy, TypeId resultTy, TypeId rootTy, NotNull<const Constraint> constraint)
{
    resultTy = follow(resultTy);

    LUAU_ASSERT(get<BlockedType>(blockedTy) && canMutate(blockedTy, constraint));

    if (blockedTy == resultTy)
    {
        rootTy = follow(rootTy);
        Scope* freeScope = nullptr;
        if (auto ft = get<FreeType>(rootTy))
            freeScope = ft->scope;
        else if (auto tt = get<TableType>(rootTy); tt && tt->state == TableState::Free)
            freeScope = tt->scope;
        else
            iceReporter.ice("bindBlockedType couldn't find an appropriate scope for a fresh type!", constraint->location);

        LUAU_ASSERT(freeScope);

        asMutable(blockedTy)->ty.emplace<BoundType>(arena->freshType(freeScope));
    }
    else
        asMutable(blockedTy)->ty.emplace<BoundType>(resultTy);
}

bool ConstraintSolver::block_(BlockedConstraintId target, NotNull<const Constraint> constraint)
{
    // If a set is not present for the target, construct a new DenseHashSet for it,
    // else grab the address of the existing set.
    auto [iter, inserted] = blocked.try_emplace(target, nullptr);
    auto& [key, blockVec] = *iter;

    if (blockVec.find(constraint))
        return false;

    blockVec.insert(constraint);

    size_t& count = blockedConstraints[constraint];
    count += 1;

    return true;
}

void ConstraintSolver::block(NotNull<const Constraint> target, NotNull<const Constraint> constraint)
{
    const bool newBlock = block_(target.get(), constraint);
    if (newBlock)
    {
        if (logger)
            logger->pushBlock(constraint, target);

        if (FFlag::DebugLuauLogSolver)
            printf("%s depends on constraint %s\n", toString(*constraint, opts).c_str(), toString(*target, opts).c_str());
    }
}

bool ConstraintSolver::block(TypeId target, NotNull<const Constraint> constraint)
{
    const bool newBlock = block_(follow(target), constraint);
    if (newBlock)
    {
        if (logger)
            logger->pushBlock(constraint, target);

        if (FFlag::DebugLuauLogSolver)
            printf("%s depends on TypeId %s\n", toString(*constraint, opts).c_str(), toString(target, opts).c_str());
    }

    return false;
}

bool ConstraintSolver::block(TypePackId target, NotNull<const Constraint> constraint)
{
    const bool newBlock = block_(target, constraint);
    if (newBlock)
    {
        if (logger)
            logger->pushBlock(constraint, target);

        if (FFlag::DebugLuauLogSolver)
            printf("%s depends on TypePackId %s\n", toString(*constraint, opts).c_str(), toString(target, opts).c_str());
    }

    return false;
}

void ConstraintSolver::inheritBlocks(NotNull<const Constraint> source, NotNull<const Constraint> addition)
{
    // Anything that is blocked on this constraint must also be blocked on our
    // synthesized constraints.
    auto blockedIt = blocked.find(source.get());
    if (blockedIt != blocked.end())
    {
        for (const Constraint* blockedConstraint : blockedIt->second)
        {
            block(addition, NotNull{blockedConstraint});
        }
    }
}

struct Blocker : TypeOnceVisitor
{
    NotNull<ConstraintSolver> solver;
    NotNull<const Constraint> constraint;

    bool blocked = false;

    explicit Blocker(NotNull<ConstraintSolver> solver, NotNull<const Constraint> constraint)
        : solver(solver)
        , constraint(constraint)
    {
    }

    bool visit(TypeId ty, const PendingExpansionType&) override
    {
        blocked = true;
        solver->block(ty, constraint);
        return false;
    }

    bool visit(TypeId ty, const ClassType&) override
    {
        return false;
    }
};

bool ConstraintSolver::blockOnPendingTypes(TypeId target, NotNull<const Constraint> constraint)
{
    Blocker blocker{NotNull{this}, constraint};
    blocker.traverse(target);
    return !blocker.blocked;
}

bool ConstraintSolver::blockOnPendingTypes(TypePackId pack, NotNull<const Constraint> constraint)
{
    Blocker blocker{NotNull{this}, constraint};
    blocker.traverse(pack);
    return !blocker.blocked;
}

void ConstraintSolver::unblock_(BlockedConstraintId progressed)
{
    auto it = blocked.find(progressed);
    if (it == blocked.end())
        return;

    // unblocked should contain a value always, because of the above check
    for (const Constraint* unblockedConstraint : it->second)
    {
        auto& count = blockedConstraints[NotNull{unblockedConstraint}];
        if (FFlag::DebugLuauLogSolver)
            printf("Unblocking count=%d\t%s\n", int(count), toString(*unblockedConstraint, opts).c_str());

        // This assertion being hit indicates that `blocked` and
        // `blockedConstraints` desynchronized at some point. This is problematic
        // because we rely on this count being correct to skip over blocked
        // constraints.
        LUAU_ASSERT(count > 0);
        count -= 1;
    }

    blocked.erase(it);
}

void ConstraintSolver::unblock(NotNull<const Constraint> progressed)
{
    if (logger)
        logger->popBlock(progressed);

    return unblock_(progressed.get());
}

void ConstraintSolver::unblock(TypeId ty, Location location)
{
    DenseHashSet<TypeId> seen{nullptr};

    TypeId progressed = ty;
    while (true)
    {
        if (seen.find(progressed))
            iceReporter.ice("ConstraintSolver::unblock encountered a self-bound type!", location);
        seen.insert(progressed);

        if (logger)
            logger->popBlock(progressed);

        unblock_(progressed);

        if (auto bt = get<BoundType>(progressed))
            progressed = bt->boundTo;
        else
            break;
    }
}

void ConstraintSolver::unblock(TypePackId progressed, Location)
{
    if (logger)
        logger->popBlock(progressed);

    return unblock_(progressed);
}

void ConstraintSolver::unblock(const std::vector<TypeId>& types, Location location)
{
    for (TypeId t : types)
        unblock(t, location);
}

void ConstraintSolver::unblock(const std::vector<TypePackId>& packs, Location location)
{
    for (TypePackId t : packs)
        unblock(t, location);
}

void ConstraintSolver::reproduceConstraints(NotNull<Scope> scope, const Location& location, const Substitution& subst)
{
    for (auto [_, newTy] : subst.newTypes)
    {
        if (get<TypeFamilyInstanceType>(newTy))
            pushConstraint(scope, location, ReduceConstraint{newTy});
    }

    for (auto [_, newPack] : subst.newPacks)
    {
        if (get<TypeFamilyInstanceTypePack>(newPack))
            pushConstraint(scope, location, ReducePackConstraint{newPack});
    }
}

bool ConstraintSolver::isBlocked(TypeId ty)
{
    ty = follow(ty);

    if (auto lt = get<LocalType>(ty))
        return lt->blockCount > 0;

    if (auto tfit = get<TypeFamilyInstanceType>(ty))
        return uninhabitedTypeFamilies.contains(ty) == false;

    return nullptr != get<BlockedType>(ty) || nullptr != get<PendingExpansionType>(ty);
}

bool ConstraintSolver::isBlocked(TypePackId tp)
{
    tp = follow(tp);

    if (auto tfitp = get<TypeFamilyInstanceTypePack>(tp))
        return uninhabitedTypeFamilies.contains(tp) == false;

    return nullptr != get<BlockedTypePack>(tp);
}

bool ConstraintSolver::isBlocked(NotNull<const Constraint> constraint)
{
    auto blockedIt = blockedConstraints.find(constraint);
    return blockedIt != blockedConstraints.end() && blockedIt->second > 0;
}

NotNull<Constraint> ConstraintSolver::pushConstraint(NotNull<Scope> scope, const Location& location, ConstraintV cv)
{
    std::unique_ptr<Constraint> c = std::make_unique<Constraint>(scope, location, std::move(cv));
    NotNull<Constraint> borrow = NotNull(c.get());
    solverConstraints.push_back(std::move(c));
    unsolvedConstraints.push_back(borrow);

    return borrow;
}

TypeId ConstraintSolver::resolveModule(const ModuleInfo& info, const Location& location)
{
    if (info.name.empty())
    {
        reportError(UnknownRequire{}, location);
        return errorRecoveryType();
    }

    for (const auto& [location, path] : requireCycles)
    {
        if (!path.empty() && path.front() == info.name)
            return builtinTypes->anyType;
    }

    ModulePtr module = moduleResolver->getModule(info.name);
    if (!module)
    {
        if (!moduleResolver->moduleExists(info.name) && !info.optional)
            reportError(UnknownRequire{moduleResolver->getHumanReadableModuleName(info.name)}, location);

        return errorRecoveryType();
    }

    if (module->type != SourceCode::Type::Module)
    {
        reportError(IllegalRequire{module->humanReadableName, "Module is not a ModuleScript. It cannot be required."}, location);
        return errorRecoveryType();
    }

    TypePackId modulePack = module->returnType;
    if (get<Unifiable::Error>(modulePack))
        return errorRecoveryType();

    std::optional<TypeId> moduleType = first(modulePack);
    if (!moduleType)
    {
        reportError(IllegalRequire{module->humanReadableName, "Module does not return exactly 1 value. It cannot be required."}, location);
        return errorRecoveryType();
    }

    return *moduleType;
}

void ConstraintSolver::reportError(TypeErrorData&& data, const Location& location)
{
    errors.emplace_back(location, std::move(data));
    errors.back().moduleName = currentModuleName;
}

void ConstraintSolver::reportError(TypeError e)
{
    errors.emplace_back(std::move(e));
    errors.back().moduleName = currentModuleName;
}

bool ConstraintSolver::hasUnresolvedConstraints(TypeId ty)
{
    if (auto refCount = unresolvedConstraints.find(ty))
        return *refCount > 0;

    return false;
}

TypeId ConstraintSolver::errorRecoveryType() const
{
    return builtinTypes->errorRecoveryType();
}

TypePackId ConstraintSolver::errorRecoveryTypePack() const
{
    return builtinTypes->errorRecoveryTypePack();
}

TypePackId ConstraintSolver::anyifyModuleReturnTypePackGenerics(TypePackId tp)
{
    tp = follow(tp);

    if (const VariadicTypePack* vtp = get<VariadicTypePack>(tp))
    {
        TypeId ty = follow(vtp->ty);
        return get<GenericType>(ty) ? builtinTypes->anyTypePack : tp;
    }

    if (!get<TypePack>(follow(tp)))
        return tp;

    std::vector<TypeId> resultTypes;
    std::optional<TypePackId> resultTail;

    TypePackIterator it = begin(tp);

    for (TypePackIterator e = end(tp); it != e; ++it)
    {
        TypeId ty = follow(*it);
        resultTypes.push_back(get<GenericType>(ty) ? builtinTypes->anyType : ty);
    }

    if (std::optional<TypePackId> tail = it.tail())
        resultTail = anyifyModuleReturnTypePackGenerics(*tail);

    return arena->addTypePack(resultTypes, resultTail);
}

LUAU_NOINLINE void ConstraintSolver::throwTimeLimitError()
{
    throw TimeLimitError(currentModuleName);
}

LUAU_NOINLINE void ConstraintSolver::throwUserCancelError()
{
    throw UserCancelError(currentModuleName);
}

// Instantiate private template implementations for external callers
template bool ConstraintSolver::unify(NotNull<const Constraint> constraint, TypeId subTy, TypeId superTy);
template bool ConstraintSolver::unify(NotNull<const Constraint> constraint, TypePackId subTy, TypePackId superTy);

} // namespace Luau
