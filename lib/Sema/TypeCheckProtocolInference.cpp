//===--- TypeCheckProtocolInference.cpp - Associated Type Inference -------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements semantic analysis for protocols, in particular, checking
// whether a given type conforms to a given protocol.
//===----------------------------------------------------------------------===//
#include "TypeCheckProtocol.h"
#include "DerivedConformances.h"
#include "TypeChecker.h"

#include "swift/AST/Decl.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Defer.h"
#include "llvm/ADT/TinyPtrVector.h"

#define DEBUG_TYPE "Associated type inference"
#include "llvm/Support/Debug.h"

using namespace swift;

void InferredAssociatedTypesByWitness::dump() const {
  dump(llvm::errs(), 0);
}

void InferredAssociatedTypesByWitness::dump(llvm::raw_ostream &out,
                                            unsigned indent) const {
  out << "\n";
  out.indent(indent) << "(";
  if (Witness) {
    Witness->dumpRef(out);
  }

  for (const auto &inferred : Inferred) {
    out << "\n";
    out.indent(indent + 2);
    out << inferred.first->getName() << " := "
        << inferred.second.getString();
  }

  for (const auto &inferred : NonViable) {
    out << "\n";
    out.indent(indent + 2);
    out << std::get<0>(inferred)->getName() << " := "
        << std::get<1>(inferred).getString();
    auto type = std::get<2>(inferred).getRequirement();
    out << " [failed constraint " << type.getString() << "]";
  }

  out << ")";
}

void InferredTypeWitnessesSolution::dump() {
  llvm::errs() << "Type Witnesses:\n";
  for (auto &typeWitness : TypeWitnesses) {
    llvm::errs() << "  " << typeWitness.first->getName() << " := ";
    typeWitness.second.first->print(llvm::errs());
    llvm::errs() << " value " << typeWitness.second.second << '\n';
  }
  llvm::errs() << "Value Witnesses:\n";
  for (unsigned i : indices(ValueWitnesses)) {
    auto &valueWitness = ValueWitnesses[i];
    llvm::errs() << i << ":  " << (Decl*)valueWitness.first
    << ' ' << valueWitness.first->getBaseName() << '\n';
    valueWitness.first->getDeclContext()->dumpContext();
    llvm::errs() << "    for " << (Decl*)valueWitness.second
    << ' ' << valueWitness.second->getBaseName() << '\n';
    valueWitness.second->getDeclContext()->dumpContext();
  }
}

namespace {
  void dumpInferredAssociatedTypesByWitnesses(
        const InferredAssociatedTypesByWitnesses &inferred,
        llvm::raw_ostream &out,
        unsigned indent) {
    for (const auto &value : inferred) {
      value.dump(out, indent);
    }
  }

  void dumpInferredAssociatedTypesByWitnesses(
        const InferredAssociatedTypesByWitnesses &inferred) LLVM_ATTRIBUTE_USED;

  void dumpInferredAssociatedTypesByWitnesses(
                          const InferredAssociatedTypesByWitnesses &inferred) {
    dumpInferredAssociatedTypesByWitnesses(inferred, llvm::errs(), 0);
  }

  void dumpInferredAssociatedTypes(const InferredAssociatedTypes &inferred,
                                   llvm::raw_ostream &out,
                                   unsigned indent) {
    for (const auto &value : inferred) {
      out << "\n";
      out.indent(indent) << "(";
      value.first->dumpRef(out);
      dumpInferredAssociatedTypesByWitnesses(value.second, out, indent + 2);
      out << ")";
    }
    out << "\n";
  }

  void dumpInferredAssociatedTypes(
         const InferredAssociatedTypes &inferred) LLVM_ATTRIBUTE_USED;

  void dumpInferredAssociatedTypes(const InferredAssociatedTypes &inferred) {
    dumpInferredAssociatedTypes(inferred, llvm::errs(), 0);
  }
}

AssociatedTypeInference::AssociatedTypeInference(
                                       TypeChecker &tc,
                                       NormalProtocolConformance *conformance)
  : tc(tc), conformance(conformance), proto(conformance->getProtocol()),
    dc(conformance->getDeclContext()),
    adoptee(conformance->getType()) { }

Type AssociatedTypeInference::computeDefaultTypeWitness(
                                              AssociatedTypeDecl *assocType) {
  // If we don't have a default definition, we're done.
  if (assocType->getDefaultDefinitionLoc().isNull())
    return Type();

  auto selfType = proto->getSelfInterfaceType();

  // Create a set of type substitutions for all known associated type.
  // FIXME: Base this on dependent types rather than archetypes?
  TypeSubstitutionMap substitutions;
  substitutions[proto->mapTypeIntoContext(selfType)
                  ->castTo<ArchetypeType>()] = dc->mapTypeIntoContext(adoptee);
  for (auto assocType : proto->getAssociatedTypeMembers()) {
    auto archetype = proto->mapTypeIntoContext(
                       assocType->getDeclaredInterfaceType())
                         ->getAs<ArchetypeType>();
    if (!archetype)
      continue;
    if (conformance->hasTypeWitness(assocType)) {
      substitutions[archetype] =
        dc->mapTypeIntoContext(
                        conformance->getTypeWitness(assocType, nullptr));
    } else {
      auto known = typeWitnesses.begin(assocType);
      if (known != typeWitnesses.end())
        substitutions[archetype] = known->first;
      else
        substitutions[archetype] = ErrorType::get(archetype);
    }
  }

  tc.validateDecl(assocType);
  Type defaultType = assocType->getDefaultDefinitionLoc().getType();

  // FIXME: Circularity
  if (!defaultType)
    return Type();

  defaultType = defaultType.subst(
                          QueryTypeSubstitutionMap{substitutions},
                          LookUpConformanceInModule(dc->getParentModule()));

  if (!defaultType)
    return Type();

  if (auto failed = checkTypeWitness(tc, dc, proto, assocType, defaultType)) {
    // Record the failure, if we haven't seen one already.
    if (!failedDefaultedAssocType) {
      failedDefaultedAssocType = assocType;
      failedDefaultedWitness = defaultType;
      failedDefaultedResult = failed;
    }

    return Type();
  }

  return defaultType;
}

Type AssociatedTypeInference::computeDerivedTypeWitness(
                                              AssociatedTypeDecl *assocType) {
  if (adoptee->hasError())
    return Type();

  // Can we derive conformances for this protocol and adoptee?
  NominalTypeDecl *derivingTypeDecl = adoptee->getAnyNominal();
  if (!DerivedConformance::derivesProtocolConformance(tc, derivingTypeDecl,
                                                      proto))
    return Type();

  // Try to derive the type witness.
  Type derivedType = tc.deriveTypeWitness(dc, derivingTypeDecl, assocType);
  if (!derivedType)
    return Type();

  // Make sure that the derived type is sane.
  if (checkTypeWitness(tc, dc, proto, assocType, derivedType)) {
    /// FIXME: Diagnose based on this.
    failedDerivedAssocType = assocType;
    failedDerivedWitness = derivedType;
    return Type();
  }

  return derivedType;
}

Type AssociatedTypeInference::substCurrentTypeWitnesses(Type type) {
  // Local function that folds dependent member types with non-dependent
  // bases into actual member references.
  std::function<Type(Type)> foldDependentMemberTypes;
  llvm::DenseSet<AssociatedTypeDecl *> recursionCheck;
  foldDependentMemberTypes = [&](Type type) -> Type {
    if (auto depMemTy = type->getAs<DependentMemberType>()) {
      auto baseTy = depMemTy->getBase().transform(foldDependentMemberTypes);
      if (baseTy.isNull() || baseTy->hasTypeParameter())
        return nullptr;

      auto assocType = depMemTy->getAssocType();
      if (!assocType)
        return nullptr;

      if (!recursionCheck.insert(assocType).second)
        return nullptr;

      SWIFT_DEFER { recursionCheck.erase(assocType); };

      // Try to substitute into the base type.
      if (Type result = depMemTy->substBaseType(dc->getParentModule(), baseTy)){
        return result;
      }

      // If that failed, check whether it's because of the conformance we're
      // evaluating.
      auto localConformance
        = tc.conformsToProtocol(
                          baseTy, assocType->getProtocol(), dc,
                          ConformanceCheckFlags::SkipConditionalRequirements);
      if (!localConformance || localConformance->isAbstract() ||
          (localConformance->getConcrete()->getRootNormalConformance()
             != conformance)) {
        return nullptr;
      }

      // Find the tentative type witness for this associated type.
      auto known = typeWitnesses.begin(assocType);
      if (known == typeWitnesses.end())
        return nullptr;

      return known->first.transform(foldDependentMemberTypes);
    }

    // The presence of a generic type parameter indicates that we
    // cannot use this type binding.
    if (type->is<GenericTypeParamType>()) {
      return nullptr;
    }

    return type;
  };

  return type.transform(foldDependentMemberTypes);
}

/// "Sanitize" requirements for conformance checking, removing any requirements
/// that unnecessarily refer to associated types of other protocols.
static void sanitizeProtocolRequirements(
                                     ProtocolDecl *proto,
                                     ArrayRef<Requirement> requirements,
                                     SmallVectorImpl<Requirement> &sanitized) {
  std::function<Type(Type)> sanitizeType;
  sanitizeType = [&](Type outerType) {
    return outerType.transformRec([&] (TypeBase *type) -> Optional<Type> {
      if (auto depMemTy = dyn_cast<DependentMemberType>(type)) {
        if (!depMemTy->getAssocType() ||
            depMemTy->getAssocType()->getProtocol() != proto) {
          for (auto member : proto->lookupDirect(depMemTy->getName())) {
            if (auto assocType = dyn_cast<AssociatedTypeDecl>(member)) {
              return Type(DependentMemberType::get(
                                           sanitizeType(depMemTy->getBase()),
                                           assocType));
            }
          }

          if (depMemTy->getBase()->is<GenericTypeParamType>())
            return Type();
        }
      }

      return None;
    });
  };

  for (const auto &req : requirements) {
    switch (req.getKind()) {
    case RequirementKind::Conformance:
    case RequirementKind::SameType:
    case RequirementKind::Superclass: {
      Type firstType = sanitizeType(req.getFirstType());
      Type secondType = sanitizeType(req.getSecondType());
      if (firstType && secondType) {
        sanitized.push_back({req.getKind(), firstType, secondType});
      }
      break;
    }

    case RequirementKind::Layout: {
      Type firstType = sanitizeType(req.getFirstType());
      if (firstType) {
        sanitized.push_back({req.getKind(), firstType,
                             req.getLayoutConstraint()});
      }
      break;
    }
    }
  }
}

bool AssociatedTypeInference::checkCurrentTypeWitnesses() {
  // Fold the dependent member types within this type.
  for (auto assocType : proto->getAssociatedTypeMembers()) {
    if (conformance->hasTypeWitness(assocType))
      continue;

    // If the type binding does not have a type parameter, there's nothing
    // to do.
    auto known = typeWitnesses.begin(assocType);
    assert(known != typeWitnesses.end());
    if (!known->first->hasTypeParameter() &&
        !known->first->hasDependentMember())
      continue;

    Type replaced = substCurrentTypeWitnesses(known->first);
    if (replaced.isNull())
      return true;

    known->first = replaced;
  }

  // If we don't have a requirement signature for this protocol, bail out.
  // FIXME: We should never get to this point. Or we should always fail.
  if (!proto->isRequirementSignatureComputed()) return false;

  // Check any same-type requirements in the protocol's requirement signature.
  SubstOptions options(None);
  options.getTentativeTypeWitness =
    [&](const NormalProtocolConformance *conformance,
        AssociatedTypeDecl *assocType) -> TypeBase * {
      if (conformance != this->conformance) return nullptr;

      auto type = typeWitnesses.begin(assocType)->first;
      return type->mapTypeOutOfContext().getPointer();
    };

  auto typeInContext = dc->mapTypeIntoContext(adoptee);

  auto substitutions =
    SubstitutionMap::getProtocolSubstitutions(
                                    proto, typeInContext,
                                    ProtocolConformanceRef(conformance));

  SmallVector<Requirement, 4> sanitizedRequirements;
  sanitizeProtocolRequirements(proto, proto->getRequirementSignature(),
                               sanitizedRequirements);
  auto result =
    tc.checkGenericArguments(dc, SourceLoc(), SourceLoc(),
                             typeInContext,
                             { proto->getProtocolSelfType() },
                             sanitizedRequirements,
                             QuerySubstitutionMap{substitutions},
                             TypeChecker::LookUpConformance(tc, dc),
                             nullptr, None, nullptr, options);
  switch (result) {
  case RequirementCheckResult::Failure:
  case RequirementCheckResult::UnsatisfiedDependency:
    return true;

  case RequirementCheckResult::Success:
  case RequirementCheckResult::SubstitutionFailure:
    return false;
  }

  return false;
}

void AssociatedTypeInference::findSolutions(
                   ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                   SmallVectorImpl<InferredTypeWitnessesSolution> &solutions) {
  SmallVector<InferredTypeWitnessesSolution, 4> nonViableSolutions;
  SmallVector<std::pair<ValueDecl *, ValueDecl *>, 4> valueWitnesses;
  findSolutionsRec(unresolvedAssocTypes, solutions, nonViableSolutions,
                   valueWitnesses, 0, 0, 0);
}

void AssociatedTypeInference::findSolutionsRec(
          ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
          SmallVectorImpl<InferredTypeWitnessesSolution> &solutions,
          SmallVectorImpl<InferredTypeWitnessesSolution> &nonViableSolutions,
          SmallVector<std::pair<ValueDecl *, ValueDecl *>, 4> &valueWitnesses,
          unsigned numTypeWitnesses,
          unsigned numValueWitnessesInProtocolExtensions,
          unsigned reqDepth) {
  typedef decltype(typeWitnesses)::ScopeTy TypeWitnessesScope;

  // If we hit the last requirement, record and check this solution.
  if (reqDepth == inferred.size()) {
    // Introduce a hash table scope; we may add type witnesses here.
    TypeWitnessesScope typeWitnessesScope(typeWitnesses);

    // Check for completeness of the solution
    for (auto assocType : unresolvedAssocTypes) {
      // Local function to record a missing associated type.
      auto recordMissing = [&] {
        if (!missingTypeWitness)
          missingTypeWitness = assocType;
      };

      auto typeWitness = typeWitnesses.begin(assocType);
      if (typeWitness != typeWitnesses.end()) {
        // The solution contains an error.
        if (typeWitness->first->hasError()) {
          recordMissing();
          return;
        }

        continue;
      }

      // We don't have a type witness for this associated type.

      // If we can form a default type, do so.
      if (Type defaultType = computeDefaultTypeWitness(assocType)) {
        if (defaultType->hasError()) {
          recordMissing();
          return;
        }

        typeWitnesses.insert(assocType, {defaultType, reqDepth});
        continue;
      }

      // If we can derive a type witness, do so.
      if (Type derivedType = computeDerivedTypeWitness(assocType)) {
        if (derivedType->hasError()) {
          recordMissing();
          return;
        }

        typeWitnesses.insert(assocType, {derivedType, reqDepth});
        continue;
      }

      // If there is a generic parameter of the named type, use that.
      if (auto gpList = dc->getGenericParamsOfContext()) {
        GenericTypeParamDecl *foundGP = nullptr;
        for (auto gp : *gpList) {
          if (gp->getName() == assocType->getName()) {
            foundGP = gp;
            break;
          }
        }

        if (foundGP) {
          auto gpType = dc->mapTypeIntoContext(
                          foundGP->getDeclaredInterfaceType());
          typeWitnesses.insert(assocType, {gpType, reqDepth});
          continue;
        }
      }

      // The solution is incomplete.
      recordMissing();
      return;
    }

    /// Check the current set of type witnesses.
    bool invalid = checkCurrentTypeWitnesses();

    // Determine whether there is already a solution with the same
    // bindings.
    for (const auto &solution : solutions) {
      bool sameSolution = true;
      for (const auto &existingTypeWitness : solution.TypeWitnesses) {
        auto typeWitness = typeWitnesses.begin(existingTypeWitness.first);
        if (!typeWitness->first->isEqual(existingTypeWitness.second.first)) {
          sameSolution = false;
          break;
        }
      }

      // We found the same solution. There is no point in recording
      // a new one.
      if (sameSolution)
        return;
    }

    auto &solutionList = invalid ? nonViableSolutions : solutions;
    solutionList.push_back(InferredTypeWitnessesSolution());
    auto &solution = solutionList.back();

    // Copy the type witnesses.
    for (auto assocType : unresolvedAssocTypes) {
      auto typeWitness = typeWitnesses.begin(assocType);
      solution.TypeWitnesses.insert({assocType, *typeWitness});
    }

    // Copy the value witnesses.
    solution.ValueWitnesses = valueWitnesses;
    solution.NumValueWitnessesInProtocolExtensions
      = numValueWitnessesInProtocolExtensions;

    // We're done recording the solution.
    return;
  }

  // Iterate over the potential witnesses for this requirement,
  // looking for solutions involving each one.
  const auto &inferredReq = inferred[reqDepth];
  for (const auto &witnessReq : inferredReq.second) {
    // Enter a new scope for the type witnesses hash table.
    TypeWitnessesScope typeWitnessesScope(typeWitnesses);
    llvm::SaveAndRestore<unsigned> savedNumTypeWitnesses(numTypeWitnesses);

    // Record this value witness, popping it when we exit the current scope.
    valueWitnesses.push_back({inferredReq.first, witnessReq.Witness});
    if (witnessReq.Witness->getDeclContext()->getAsProtocolExtensionContext())
      ++numValueWitnessesInProtocolExtensions;
    SWIFT_DEFER {
      if (witnessReq.Witness->getDeclContext()->getAsProtocolExtensionContext())
        --numValueWitnessesInProtocolExtensions;
      valueWitnesses.pop_back();
    };

    // Introduce each of the type witnesses into the hash table.
    bool failed = false;
    for (const auto &typeWitness : witnessReq.Inferred) {
      // If we've seen a type witness for this associated type that
      // conflicts, there is no solution.
      auto known = typeWitnesses.begin(typeWitness.first);
      if (known != typeWitnesses.end()) {
        // If witnesses for two difference requirements inferred the same
        // type, we're okay.
        if (known->first->isEqual(typeWitness.second))
          continue;

        // If one has a type parameter remaining but the other does not,
        // drop the one with the type parameter.
        if ((known->first->hasTypeParameter() ||
             known->first->hasDependentMember())
            != (typeWitness.second->hasTypeParameter() ||
                typeWitness.second->hasDependentMember())) {
          if (typeWitness.second->hasTypeParameter() ||
              typeWitness.second->hasDependentMember())
            continue;

          known->first = typeWitness.second;
          continue;
        }

        if (!typeWitnessConflict ||
            numTypeWitnesses > numTypeWitnessesBeforeConflict) {
          typeWitnessConflict = {typeWitness.first,
                                 typeWitness.second,
                                 inferredReq.first,
                                 witnessReq.Witness,
                                 known->first,
                                 valueWitnesses[known->second].first,
                                 valueWitnesses[known->second].second};
          numTypeWitnessesBeforeConflict = numTypeWitnesses;
        }

        failed = true;
        break;
      }

      // Record the type witness.
      ++numTypeWitnesses;
      typeWitnesses.insert(typeWitness.first, {typeWitness.second, reqDepth});
    }

    if (failed)
      continue;

    // Recurse
    findSolutionsRec(unresolvedAssocTypes, solutions, nonViableSolutions,
                     valueWitnesses, numTypeWitnesses,
                     numValueWitnessesInProtocolExtensions, reqDepth + 1);
  }
}

static Comparison
compareDeclsForInference(TypeChecker &TC, DeclContext *DC,
                         ValueDecl *decl1, ValueDecl *decl2) {
  // TC.compareDeclarations assumes that it's comparing two decls that
  // apply equally well to a call site. We haven't yet inferred the
  // associated types for a type, so the ranking algorithm used by
  // compareDeclarations to score protocol extensions is inappropriate,
  // since we may have potential witnesses from extensions with mutually
  // exclusive associated type constraints, and compareDeclarations will
  // consider these unordered since neither extension's generic signature
  // is a superset of the other.

  // If the witnesses come from the same decl context, score normally.
  auto dc1 = decl1->getDeclContext();
  auto dc2 = decl2->getDeclContext();

  if (dc1 == dc2)
    return TC.compareDeclarations(DC, decl1, decl2);

  auto isProtocolExt1 =
    (bool)dc1->getAsProtocolExtensionContext();
  auto isProtocolExt2 =
    (bool)dc2->getAsProtocolExtensionContext();

  // If one witness comes from a protocol extension, favor the one
  // from a concrete context.
  if (isProtocolExt1 != isProtocolExt2) {
    return isProtocolExt1 ? Comparison::Worse : Comparison::Better;
  }

  // If both witnesses came from concrete contexts, score normally.
  // Associated type inference shouldn't impact the result.
  // FIXME: It could, if someone constrained to ConcreteType.AssocType...
  if (!isProtocolExt1)
    return TC.compareDeclarations(DC, decl1, decl2);

  // Compare protocol extensions by which protocols they require Self to
  // conform to. If one extension requires a superset of the other's
  // constraints, it wins.
  auto sig1 = dc1->getGenericSignatureOfContext();
  auto sig2 = dc2->getGenericSignatureOfContext();

  // FIXME: Extensions sometimes have null generic signatures while
  // checking the standard library...
  if (!sig1 || !sig2)
    return TC.compareDeclarations(DC, decl1, decl2);

  auto selfParam = GenericTypeParamType::get(0, 0, TC.Context);

  // Collect the protocols required by extension 1.
  Type class1;
  SmallPtrSet<ProtocolDecl*, 4> protos1;

  std::function<void (ProtocolDecl*)> insertProtocol;
  insertProtocol = [&](ProtocolDecl *p) {
    if (!protos1.insert(p).second)
      return;

    for (auto parent : p->getInheritedProtocols())
      insertProtocol(parent);
  };

  for (auto &reqt : sig1->getRequirements()) {
    if (!reqt.getFirstType()->isEqual(selfParam))
      continue;
    switch (reqt.getKind()) {
    case RequirementKind::Conformance: {
      auto *proto = reqt.getSecondType()->castTo<ProtocolType>()->getDecl();
      insertProtocol(proto);
      break;
    }
    case RequirementKind::Superclass:
      class1 = reqt.getSecondType();
      break;

    case RequirementKind::SameType:
    case RequirementKind::Layout:
      break;
    }
  }

  // Compare with the protocols required by extension 2.
  Type class2;
  SmallPtrSet<ProtocolDecl*, 4> protos2;
  bool protos2AreSubsetOf1 = true;
  std::function<void (ProtocolDecl*)> removeProtocol;
  removeProtocol = [&](ProtocolDecl *p) {
    if (!protos2.insert(p).second)
      return;

    protos2AreSubsetOf1 &= protos1.erase(p);
    for (auto parent : p->getInheritedProtocols())
      removeProtocol(parent);
  };

  for (auto &reqt : sig2->getRequirements()) {
    if (!reqt.getFirstType()->isEqual(selfParam))
      continue;
    switch (reqt.getKind()) {
    case RequirementKind::Conformance: {
      auto *proto = reqt.getSecondType()->castTo<ProtocolType>()->getDecl();
      removeProtocol(proto);
      break;
    }
    case RequirementKind::Superclass:
      class2 = reqt.getSecondType();
      break;

    case RequirementKind::SameType:
    case RequirementKind::Layout:
      break;
    }
  }

  auto isClassConstraintAsStrict = [&](Type t1, Type t2) -> bool {
    if (!t1)
      return !t2;

    if (!t2)
      return true;

    return TC.isSubclassOf(t1, t2, DC);
  };

  bool protos1AreSubsetOf2 = protos1.empty();
  // If the second extension requires strictly more protocols than the
  // first, it's better.
  if (protos1AreSubsetOf2 > protos2AreSubsetOf1
      && isClassConstraintAsStrict(class2, class1)) {
    return Comparison::Worse;
  // If the first extension requires strictly more protocols than the
  // second, it's better.
  } else if (protos2AreSubsetOf1 > protos1AreSubsetOf2
             && isClassConstraintAsStrict(class1, class2)) {
    return Comparison::Better;
  }

  // If they require the same set of protocols, or non-overlapping
  // sets, judge them normally.
  return TC.compareDeclarations(DC, decl1, decl2);
}

bool AssociatedTypeInference::isBetterSolution(
                      const InferredTypeWitnessesSolution &first,
                      const InferredTypeWitnessesSolution &second) {
  assert(first.ValueWitnesses.size() == second.ValueWitnesses.size());
  bool firstBetter = false;
  bool secondBetter = false;
  for (unsigned i = 0, n = first.ValueWitnesses.size(); i != n; ++i) {
    assert(first.ValueWitnesses[i].first == second.ValueWitnesses[i].first);
    auto firstWitness = first.ValueWitnesses[i].second;
    auto secondWitness = second.ValueWitnesses[i].second;
    if (firstWitness == secondWitness)
      continue;

    switch (compareDeclsForInference(tc, dc, firstWitness, secondWitness)) {
    case Comparison::Better:
      if (secondBetter)
        return false;

      firstBetter = true;
      break;

    case Comparison::Worse:
      if (firstBetter)
        return false;

      secondBetter = true;
      break;

    case Comparison::Unordered:
      break;
    }
  }

  return firstBetter;
}

bool AssociatedTypeInference::findBestSolution(
                   SmallVectorImpl<InferredTypeWitnessesSolution> &solutions) {
  if (solutions.empty()) return true;
  if (solutions.size() == 1) return false;

  // Find the smallest number of value witnesses found in protocol extensions.
  // FIXME: This is a silly heuristic that should go away.
  unsigned bestNumValueWitnessesInProtocolExtensions
    = solutions.front().NumValueWitnessesInProtocolExtensions;
  for (unsigned i = 1, n = solutions.size(); i != n; ++i) {
    bestNumValueWitnessesInProtocolExtensions
      = std::min(bestNumValueWitnessesInProtocolExtensions,
                 solutions[i].NumValueWitnessesInProtocolExtensions);
  }

  // Erase any solutions with more value witnesses in protocol
  // extensions than the best.
  solutions.erase(
    std::remove_if(solutions.begin(), solutions.end(),
                   [&](const InferredTypeWitnessesSolution &solution) {
                     return solution.NumValueWitnessesInProtocolExtensions >
                              bestNumValueWitnessesInProtocolExtensions;
                   }),
    solutions.end());

  // If we're down to one solution, success!
  if (solutions.size() == 1) return false;

  // Find a solution that's at least as good as the solutions that follow it.
  unsigned bestIdx = 0;
  for (unsigned i = 1, n = solutions.size(); i != n; ++i) {
    if (isBetterSolution(solutions[i], solutions[bestIdx]))
      bestIdx = i;
  }

  // Make sure that solution is better than any of the other solutions.
  bool ambiguous = false;
  for (unsigned i = 1, n = solutions.size(); i != n; ++i) {
    if (i != bestIdx && !isBetterSolution(solutions[bestIdx], solutions[i])) {
      ambiguous = true;
      break;
    }
  }

  // If the result was ambiguous, fail.
  if (ambiguous) {
    assert(solutions.size() != 1 && "should have succeeded somewhere above?");
    return true;

  }
  // Keep the best solution, erasing all others.
  if (bestIdx != 0)
    solutions[0] = std::move(solutions[bestIdx]);
  solutions.erase(solutions.begin() + 1, solutions.end());
  return false;
}

namespace {
  /// A failed type witness binding.
  struct FailedTypeWitness {
    /// The value requirement that triggered inference.
    ValueDecl *Requirement;

    /// The corresponding value witness from which the type witness
    /// was inferred.
    ValueDecl *Witness;

    /// The actual type witness that was inferred.
    Type TypeWitness;

    /// The failed type witness result.
    CheckTypeWitnessResult Result;
  };
} // end anonymous namespace

bool AssociatedTypeInference::diagnoseNoSolutions(
                         ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                         ConformanceChecker &checker) {
  // If a defaulted type witness failed, diagnose it.
  if (failedDefaultedAssocType) {
    auto failedDefaultedAssocType = this->failedDefaultedAssocType;
    auto failedDefaultedWitness = this->failedDefaultedWitness;
    auto failedDefaultedResult = this->failedDefaultedResult;

    checker.diagnoseOrDefer(failedDefaultedAssocType, true,
      [failedDefaultedAssocType, failedDefaultedWitness,
       failedDefaultedResult](NormalProtocolConformance *conformance) {
        auto proto = conformance->getProtocol();
        auto &diags = proto->getASTContext().Diags;
        diags.diagnose(failedDefaultedAssocType,
                       diag::default_associated_type_req_fail,
                       failedDefaultedWitness,
                       failedDefaultedAssocType->getFullName(),
                       proto->getDeclaredType(),
                       failedDefaultedResult.getRequirement(),
                       failedDefaultedResult.isConformanceRequirement());
      });

    return true;
  }

  // Form a mapping from associated type declarations to failed type
  // witnesses.
  llvm::DenseMap<AssociatedTypeDecl *, SmallVector<FailedTypeWitness, 2>>
    failedTypeWitnesses;
  for (const auto &inferredReq : inferred) {
    for (const auto &inferredWitness : inferredReq.second) {
      for (const auto &nonViable : inferredWitness.NonViable) {
        failedTypeWitnesses[std::get<0>(nonViable)]
          .push_back({inferredReq.first, inferredWitness.Witness,
                      std::get<1>(nonViable), std::get<2>(nonViable)});
      }
    }
  }

  // Local function to attempt to diagnose potential type witnesses
  // that failed requirements.
  auto tryDiagnoseTypeWitness = [&](AssociatedTypeDecl *assocType) -> bool {
    auto known = failedTypeWitnesses.find(assocType);
    if (known == failedTypeWitnesses.end())
      return false;

    auto failedSet = std::move(known->second);
    checker.diagnoseOrDefer(assocType, true,
      [assocType, failedSet](NormalProtocolConformance *conformance) {
        auto proto = conformance->getProtocol();
        auto &diags = proto->getASTContext().Diags;
        diags.diagnose(assocType, diag::bad_associated_type_deduction,
                       assocType->getFullName(), proto->getFullName());
        for (const auto &failed : failedSet) {
          diags.diagnose(failed.Witness,
                         diag::associated_type_deduction_witness_failed,
                         failed.Requirement->getFullName(),
                         failed.TypeWitness,
                         failed.Result.getRequirement(),
                         failed.Result.isConformanceRequirement());
        }
      });

    return true;
  };

  // Try to diagnose the first missing type witness we encountered.
  if (missingTypeWitness && tryDiagnoseTypeWitness(missingTypeWitness))
    return true;

  // Failing that, try to diagnose any type witness that failed a
  // requirement.
  for (auto assocType : unresolvedAssocTypes) {
    if (tryDiagnoseTypeWitness(assocType))
      return true;
  }

  // If we saw a conflict, complain about it.
  if (typeWitnessConflict) {
    auto typeWitnessConflict = this->typeWitnessConflict;

    checker.diagnoseOrDefer(typeWitnessConflict->AssocType, true,
      [typeWitnessConflict](NormalProtocolConformance *conformance) {
        auto &diags = conformance->getDeclContext()->getASTContext().Diags;
        diags.diagnose(typeWitnessConflict->AssocType,
                       diag::ambiguous_associated_type_deduction,
                       typeWitnessConflict->AssocType->getFullName(),
                       typeWitnessConflict->FirstType,
                       typeWitnessConflict->SecondType);

        diags.diagnose(typeWitnessConflict->FirstWitness,
                       diag::associated_type_deduction_witness,
                       typeWitnessConflict->FirstRequirement->getFullName(),
                       typeWitnessConflict->FirstType);
        diags.diagnose(typeWitnessConflict->SecondWitness,
                       diag::associated_type_deduction_witness,
                       typeWitnessConflict->SecondRequirement->getFullName(),
                       typeWitnessConflict->SecondType);
      });

    return true;
  }

  return false;
}

bool AssociatedTypeInference::diagnoseAmbiguousSolutions(
                  ArrayRef<AssociatedTypeDecl *> unresolvedAssocTypes,
                  ConformanceChecker &checker,
                  SmallVectorImpl<InferredTypeWitnessesSolution> &solutions) {
  for (auto assocType : unresolvedAssocTypes) {
    // Find two types that conflict.
    auto &firstSolution = solutions.front();

    // Local function to retrieve the value witness for the current associated
    // type within the given solution.
    auto getValueWitness = [&](InferredTypeWitnessesSolution &solution) {
      unsigned witnessIdx = solution.TypeWitnesses[assocType].second;
      if (witnessIdx < solution.ValueWitnesses.size())
        return solution.ValueWitnesses[witnessIdx];

      return std::pair<ValueDecl *, ValueDecl *>(nullptr, nullptr);
    };

    Type firstType = firstSolution.TypeWitnesses[assocType].first;

    // Extract the value witness used to deduce this associated type, if any.
    auto firstMatch = getValueWitness(firstSolution);

    Type secondType;
    std::pair<ValueDecl *, ValueDecl *> secondMatch;
    for (auto &solution : solutions) {
      Type typeWitness = solution.TypeWitnesses[assocType].first;
      if (!typeWitness->isEqual(firstType)) {
        secondType = typeWitness;
        secondMatch = getValueWitness(solution);
        break;
      }
    }

    if (!secondType)
      continue;

    // We found an ambiguity. diagnose it.
    checker.diagnoseOrDefer(assocType, true,
      [assocType, firstType, firstMatch, secondType, secondMatch](
        NormalProtocolConformance *conformance) {
        auto &diags = assocType->getASTContext().Diags;
        diags.diagnose(assocType, diag::ambiguous_associated_type_deduction,
                       assocType->getFullName(), firstType, secondType);

        auto diagnoseWitness = [&](std::pair<ValueDecl *, ValueDecl *> match,
                                   Type type){
          // If we have a requirement/witness pair, diagnose it.
          if (match.first && match.second) {
            diags.diagnose(match.second,
                           diag::associated_type_deduction_witness,
                           match.first->getFullName(), type);

            return;
          }

          // Otherwise, we have a default.
          diags.diagnose(assocType, diag::associated_type_deduction_default,
                         type)
            .highlight(assocType->getDefaultDefinitionLoc().getSourceRange());
        };

        diagnoseWitness(firstMatch, firstType);
        diagnoseWitness(secondMatch, secondType);
      });

    return true;
  }

  return false;
}

auto AssociatedTypeInference::solve(ConformanceChecker &checker)
    -> Optional<InferredTypeWitnesses> {
  // Track when we are checking type witnesses.
  ProtocolConformanceState initialState = conformance->getState();
  conformance->setState(ProtocolConformanceState::CheckingTypeWitnesses);
  SWIFT_DEFER { conformance->setState(initialState); };

  // Try to resolve type witnesses via name lookup.
  llvm::SetVector<AssociatedTypeDecl *> unresolvedAssocTypes;
  for (auto assocType : proto->getAssociatedTypeMembers()) {
    // If we already have a type witness, do nothing.
    if (conformance->hasTypeWitness(assocType))
      continue;

    // Try to resolve this type witness via name lookup, which is the
    // most direct mechanism, overriding all others.
    switch (checker.resolveTypeWitnessViaLookup(assocType)) {
    case ResolveWitnessResult::Success:
      // Success. Move on to the next.
      continue;

    case ResolveWitnessResult::ExplicitFailed:
      continue;

    case ResolveWitnessResult::Missing:
      // Note that we haven't resolved this associated type yet.
      unresolvedAssocTypes.insert(assocType);
      break;
    }
  }

  // Result variable to use for returns so that we get NRVO.
  Optional<InferredTypeWitnesses> result;
  result = { };

  // If we resolved everything, we're done.
  if (unresolvedAssocTypes.empty())
    return result;

  // Infer potential type witnesses from value witnesses.
  inferred = checker.inferTypeWitnessesViaValueWitnesses(unresolvedAssocTypes);
  DEBUG(llvm::dbgs() << "Candidates for inference:\n";
        dumpInferredAssociatedTypes(inferred));

  // Compute the set of solutions.
  SmallVector<InferredTypeWitnessesSolution, 4> solutions;
  findSolutions(unresolvedAssocTypes.getArrayRef(), solutions);

  // Go make sure that type declarations that would act as witnesses
  // did not get injected while we were performing checks above. This
  // can happen when two associated types in different protocols have
  // the same name, and validating a declaration (above) triggers the
  // type-witness generation for that second protocol, introducing a
  // new type declaration.
  // FIXME: This is ridiculous.
  for (auto assocType : unresolvedAssocTypes) {
    switch (checker.resolveTypeWitnessViaLookup(assocType)) {
    case ResolveWitnessResult::Success:
    case ResolveWitnessResult::ExplicitFailed:
      // A declaration that can become a witness has shown up. Go
      // perform the resolution again now that we have more
      // information.
      return solve(checker);

    case ResolveWitnessResult::Missing:
      // The type witness is still missing. Keep going.
      break;
    }
  }

  // Find the best solution.
  if (!findBestSolution(solutions)) {
    assert(solutions.size() == 1 && "Not a unique best solution?");
    // Form the resulting solution.
    auto &typeWitnesses = solutions.front().TypeWitnesses;
    for (auto assocType : unresolvedAssocTypes) {
      assert(typeWitnesses.count(assocType) == 1 && "missing witness");
      auto replacement = typeWitnesses[assocType].first;
      // FIXME: We can end up here with dependent types that were not folded
      // away for some reason.
      if (replacement->hasDependentMember())
        return None;

      if (replacement->hasArchetype())
        replacement = replacement->mapTypeOutOfContext();

      result->push_back({assocType, replacement});
    }

    return result;
  }

  // Diagnose the complete lack of solutions.
  if (solutions.empty() &&
      diagnoseNoSolutions(unresolvedAssocTypes.getArrayRef(), checker))
    return None;

  // Diagnose ambiguous solutions.
  if (!solutions.empty() &&
      diagnoseAmbiguousSolutions(unresolvedAssocTypes.getArrayRef(), checker,
                                 solutions))
    return None;

  // Save the missing type witnesses for later diagnosis.
  checker.GlobalMissingWitnesses.insert(unresolvedAssocTypes.begin(),
                                        unresolvedAssocTypes.end());
  return None;
}

void ConformanceChecker::resolveTypeWitnesses() {
  SWIFT_DEFER {
    // Resolution attempts to have the witnesses be correct by construction, but
    // this isn't guaranteed, so let's double check.
    ensureRequirementsAreSatisfied();
  };

  // Attempt to infer associated type witnesses.
  AssociatedTypeInference inference(TC, Conformance);
  if (auto inferred = inference.solve(*this)) {
    for (const auto &inferredWitness : *inferred) {
      recordTypeWitness(inferredWitness.first, inferredWitness.second,
                        /*typeDecl=*/nullptr,
      /*performRedeclarationCheck=*/true);
    }

    ensureRequirementsAreSatisfied();
    return;
  }

  // Conformance failed. Record errors for each of the witnesses.
  Conformance->setInvalid();

  // We're going to produce an error below. Mark each unresolved
  // associated type witness as erroneous.
  for (auto assocType : Proto->getAssociatedTypeMembers()) {
    // If we already have a type witness, do nothing.
    if (Conformance->hasTypeWitness(assocType))
      continue;

    recordTypeWitness(assocType, ErrorType::get(TC.Context), nullptr, true);
  }

  return;

  // Multiple solutions. Diagnose the ambiguity.

}

void ConformanceChecker::resolveSingleTypeWitness(
       AssociatedTypeDecl *assocType) {
  // Ensure we diagnose if the witness is missing.
  SWIFT_DEFER {
    diagnoseMissingWitnesses(MissingWitnessDiagnosisKind::ErrorFixIt);
  };
  switch (resolveTypeWitnessViaLookup(assocType)) {
  case ResolveWitnessResult::Success:
  case ResolveWitnessResult::ExplicitFailed:
    // We resolved this type witness one way or another.
    return;

  case ResolveWitnessResult::Missing:
    // The type witness is still missing. Resolve all of the type witnesses.
    resolveTypeWitnesses();
    return;
  }
}

void ConformanceChecker::resolveSingleWitness(ValueDecl *requirement) {
  assert(!isa<AssociatedTypeDecl>(requirement) && "Not a value witness");
  assert(!Conformance->hasWitness(requirement) && "Already resolved");

  // Note that we're resolving this witness.
  assert(ResolvingWitnesses.count(requirement) == 0 && "Currently resolving");
  ResolvingWitnesses.insert(requirement);
  SWIFT_DEFER { ResolvingWitnesses.erase(requirement); };

  // Make sure we've validated the requirement.
  if (!requirement->hasInterfaceType())
    TC.validateDecl(requirement);

  if (requirement->isInvalid() || !requirement->hasValidSignature()) {
    Conformance->setInvalid();
    return;
  }

  if (!requirement->isProtocolRequirement())
    return;

  // Resolve all associated types before trying to resolve this witness.
  resolveTypeWitnesses();

  // If any of the type witnesses was erroneous, don't bother to check
  // this value witness: it will fail.
  for (auto assocType : getReferencedAssociatedTypes(requirement)) {
    if (Conformance->getTypeWitness(assocType, nullptr)->hasError()) {
      Conformance->setInvalid();
      return;
    }
  }

  // Try to resolve the witness via explicit definitions.
  switch (resolveWitnessViaLookup(requirement)) {
  case ResolveWitnessResult::Success:
    return;

  case ResolveWitnessResult::ExplicitFailed:
    Conformance->setInvalid();
    recordInvalidWitness(requirement);
    return;

  case ResolveWitnessResult::Missing:
    // Continue trying below.
    break;
  }

  // Try to resolve the witness via derivation.
  switch (resolveWitnessViaDerivation(requirement)) {
  case ResolveWitnessResult::Success:
    return;

  case ResolveWitnessResult::ExplicitFailed:
    Conformance->setInvalid();
    recordInvalidWitness(requirement);
    return;

  case ResolveWitnessResult::Missing:
    // Continue trying below.
    break;
  }

  // Try to resolve the witness via defaults.
  switch (resolveWitnessViaDefault(requirement)) {
  case ResolveWitnessResult::Success:
    return;

  case ResolveWitnessResult::ExplicitFailed:
    Conformance->setInvalid();
    recordInvalidWitness(requirement);
    return;

  case ResolveWitnessResult::Missing:
    llvm_unreachable("Should have failed");
    break;
  }
}
