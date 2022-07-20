//===- DIDependencyAnalysis.cpp - Dependency Analyzer (Metadata) *- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements passes which uses source-level debug information
// to summarize low-level results of privatization, reduction and induction
// recognition and flow/anti/output dependencies exploration.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "BitMemoryTrait.h"
#include "tsar/ADT/PersistentMap.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/PrivateAnalysis.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Tags.h"
#include "tsar/Support/Utils.h"
#include "tsar/Unparse/SourceUnparser.h"
#include "tsar/Unparse/Utils.h"
#include <bcl/tagged.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/DiagnosticInfo.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/DebugInfo.h>
#include <tuple>
#include <utility>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "da-di"

MEMORY_TRAIT_STATISTIC(NumTraits)

char DIDependencyAnalysisPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(DIDependencyAnalysisPass, "da-di",
  "Dependency Analysis (Metadata)", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(LiveMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass);
  INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass);
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass);
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass);
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper);
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper);
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_IN_GROUP_END(DIDependencyAnalysisPass, "da-di",
  "Dependency Analysis (Metadata)", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

namespace {
void allocatePoolLog(unsigned DWLang,
    std::unique_ptr<DIMemoryTraitRegionPool> &Pool) {
  if (!Pool) {
    dbgs() << "[DA DI]: allocate pool of region traits\n";
    return;
  }
  dbgs() << "[DA DI]: pool of region traits already allocated: ";
  for (auto &M : *Pool)
    printDILocationSource(DWLang, *M.getMemory(), dbgs()), dbgs() << " ";
  dbgs() << "\n";
}

/// Return `true` if traits for a specified memory should not be updated.
///
/// `LockedTraits` is a list of traits which is locked. All memory locations
/// which may alias with a memory from this list also should be locked.
///
/// \attention We do not set `trait::Lock` property for `T` if it should be
/// locked implicitly (if it does not already contained in LockedTraits).
/// If we set this property and add `T` in LockedTraits then some traits which
/// should not be locked may become locked. For example, let us consider a
/// structure S with fields X and Y. If S.X should be locked then S should
/// be locked implicitly and S.Y should not be locked. However, if we mark
/// S as locked and store it in LockedTraits then S.Y becomes implicitly
/// locked.
bool isLockedTrait(const DIMemoryTrait &T,
  ArrayRef<const DIMemory *> LockedTraits,
  const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR) {
  if (T.is<trait::Lock>())
    return true;
  auto *DIM = T.getMemory();
  auto *AN = DIM->getAliasNode();
  for (auto *M : LockedTraits) {
    if (!DIAliasSTR.isUnreachable(AN, M->getAliasNode()))
      return true;
  }
  return false;
}

/// Move representation of a dependence of a specified type `Tag` to
/// metadata-level representation if it is more accurate.
///
/// \pre `Tag` must be one of `trait::Flow`, `trait::Anti`, `trait::Output`.
template<class Tag> void moveIf(
    DIMemoryTrait &FromDIMTrait, DIMemoryTrait &DIMTrait) {
  static_assert(std::is_same<Tag, trait::Flow>::value ||
    std::is_same<Tag, trait::Anti>::value ||
    std::is_same<Tag, trait::Output>::value, "Unknown type of dependence!");
  if (!DIMTrait.is<Tag>())
    return;
  if (auto *FromDep = FromDIMTrait.template get<Tag>()) {
    if (auto *DIDep = DIMTrait.template get<Tag>())
      if (DIDep->isKnownDistance())
        return;
    LLVM_DEBUG(dbgs() << "[DA DI]: move " << Tag::toString()
                      << " dependence\n");
    DIMTrait.template set<Tag>(FromDIMTrait.template release<Tag>());
  }
}

/// Combine IR-level representation of a dependence of a specified type `Tag`
/// with a specified metadata-level representation.
///
/// \pre `Tag` must be one of `trait::Flow`, `trait::Anti`, `trati::Output`.
template<class Tag> void combineIf(
    const EstimateMemoryTrait &IRTrait, DIMemoryTrait &DITrait) {
  static_assert(std::is_same<Tag, trait::Flow>::value ||
    std::is_same<Tag, trait::Anti>::value ||
    std::is_same<Tag, trait::Output>::value, "Unknown type of dependence!");
  auto *IRDep = IRTrait.template get<Tag>();
  auto *DIDep = DITrait.template get<Tag>();
  LLVM_DEBUG(if (IRDep || DIDep)
    dbgs() << "[DA DI]: combine " << Tag::toString() << " dependence\n");
  if (IRDep) {
    SmallVector<trait::DIDependence::Cause, 4> Causes;
    for (auto *V : IRDep->getCauses()) {
      trait::DIDependence::Cause C;
      if (auto *I = dyn_cast<Instruction>(V)) {
        C.get<DebugLoc>() = I->getDebugLoc();
        if (auto *CB = dyn_cast<CallBase>(I))
          if (auto F = llvm::dyn_cast<Function>(
                  CB->getCalledOperand()->stripPointerCasts()))
            if (auto DISub = findMetadata(F))
              C.get<ObjectID>() = DISub;
      }
      if (C.get<DebugLoc>() || C.get<ObjectID>())
        Causes.push_back(std::move(C));
    }
    LLVM_DEBUG(dbgs() << "[DA DI]: the number of known causes is "
                      << Causes.size() << "\n");
    if (!DIDep) {
      if (!hasSpuriousDep(DITrait) && !DITrait.template is<Tag>()) {
        trait::DIDependence::DistanceVector DIDistVector(IRDep->getLevels());
        for (unsigned I = 0, EI = IRDep->getLevels(); I < EI; ++I) {
          auto Dist = IRDep->getDistance(I);
          if (auto ConstDist = dyn_cast_or_null<SCEVConstant>(Dist.first))
            DIDistVector[I].first = APSInt(ConstDist->getAPInt(), I == 0);
          if (auto ConstDist = dyn_cast_or_null<SCEVConstant>(Dist.second))
            DIDistVector[I].second = APSInt(ConstDist->getAPInt(), I == 0);
        }
        auto KnownSize = DIDistVector.size();
        for (auto I = DIDistVector.rbegin(), EI = DIDistVector.rend(); I != EI;
             ++I, --KnownSize)
          if (I->first || I->second)
            break;
        DIDistVector.resize(KnownSize);
        DITrait.template set<Tag>(
          new trait::DIDependence(IRDep->getFlags(), DIDistVector, Causes));
      } else {
        auto F = IRDep->getFlags() | trait::Dependence::UnknownCause;
        if (F & trait::Dependence::May)
          F &= ~trait::Dependence::May;
        DITrait.template set<Tag>(new trait::DIDependence(F, Causes));
      }
    } else {
      auto F = IRDep->getFlags() | DIDep->getFlags();
      if (!IRDep->isMay() || !DIDep->isMay())
        F &= ~trait::Dependence::May;
      trait::DIDependence::DistanceVector DIDistVector(IRDep->getLevels());
      for (unsigned I = 0, EI = IRDep->getLevels(); I < EI; ++I) {
        auto Dist = IRDep->getDistance(I);
        if (auto ConstDist = dyn_cast_or_null<SCEVConstant>(Dist.first))
          if (DIDep->getDistance(I).first)
            DIDistVector[I].first = APSInt(ConstDist->getAPInt(), I == 0) <=
                                            DIDep->getDistance(I).first
                                        ? APSInt(ConstDist->getAPInt(), I == 0)
                                        : DIDep->getDistance(I).first;
        if (auto ConstDist = dyn_cast_or_null<SCEVConstant>(Dist.second))
          if (DIDep->getDistance(I).second)
            DIDistVector[I].second = APSInt(ConstDist->getAPInt(), I == 0) >=
                                             DIDep->getDistance(I).second
                                         ? APSInt(ConstDist->getAPInt(), I == 0)
                                         : DIDep->getDistance(I).second;
      }
      auto KnownSize = DIDistVector.size();
      for (auto I = DIDistVector.rbegin(), EI = DIDistVector.rend(); I != EI;
           ++I, --KnownSize)
        if (I->first || I->second)
          break;
      DIDistVector.resize(KnownSize);
      DITrait.template set<Tag>(
          new trait::DIDependence(F, DIDistVector, Causes));
    }
  } else if (DIDep && (hasSpuriousDep(IRTrait) || IRTrait.template is<Tag>())) {
    auto F = DIDep->getFlags() | trait::Dependence::UnknownCause;
    if (F & trait::Dependence::May)
      F &= ~trait::Dependence::May;
    DITrait.template set<Tag>(new trait::DIDependence(F));
  }
}

/// Combine two metadata-level representations of a dependence of a specified
/// type `Tag`.
///
/// \pre `Tag` must be one of `trait::Flow`, `trait::Anti`, `trati::Output`.
template<class Tag> void combineIf(
    const DIMemoryTrait &FromDITrait, DIMemoryTrait &DITrait) {
  static_assert(std::is_same<Tag, trait::Flow>::value ||
    std::is_same<Tag, trait::Anti>::value ||
    std::is_same<Tag, trait::Output>::value, "Unknown type of dependence!");
  auto *FromDIDep = FromDITrait.template get<Tag>();
  auto *DIDep = DITrait.template get<Tag>();
  LLVM_DEBUG(if (FromDIDep || DIDep)
    dbgs() << "[DA DI]: combine " << Tag::toString() << " dependence\n");
  if (FromDIDep) {
    if (!DIDep) {
      if (!hasSpuriousDep(DITrait) && !DITrait.template is<Tag>()) {
        DITrait.template set<Tag>(new trait::DIDependence(*FromDIDep));
      } else {
        auto F = FromDIDep->getFlags() | trait::Dependence::UnknownCause;
        if (F & trait::Dependence::May)
          F &= ~trait::Dependence::May;
        DITrait.template set<Tag>(
            new trait::DIDependence(F, FromDIDep->getCauses()));
      }
    } else {
      auto F = FromDIDep->getFlags() | DIDep->getFlags();
      if (!FromDIDep->isMay() || !DIDep->isMay())
        F &= ~trait::Dependence::May;
      trait::DIDependence::DistanceVector DIDistVector(
          std::min(FromDIDep->getLevels(), DIDep->getLevels()));
      for (unsigned I = 0, EI = DIDistVector.size(); I < EI; ++I) {
        if (FromDIDep->getDistance(I).first && DIDep->getDistance(I).first)
          DIDistVector[I].first =
              FromDIDep->getDistance(I).first <= DIDep->getDistance(I).first
                  ? FromDIDep->getDistance(I).first
                  : DIDep->getDistance(I).first;
        if (FromDIDep->getDistance(I).second && DIDep->getDistance(I).second)
          DIDistVector[I].second =
              FromDIDep->getDistance(I).second >= DIDep->getDistance(I).second
                  ? FromDIDep->getDistance(I).second
                  : DIDep->getDistance(I).second;
      }
      auto KnownSize = DIDistVector.size();
      for (auto I = DIDistVector.rbegin(), EI = DIDistVector.rend(); I != EI;
           ++I, --KnownSize)
        if (I->first || I->second)
          break;
      DIDistVector.resize(KnownSize);
      DITrait.template set<Tag>(
          new trait::DIDependence(F, DIDistVector, FromDIDep->getCauses()));
    }
  } else if (DIDep && (hasSpuriousDep(FromDITrait) ||
             FromDITrait.template is<Tag>())) {
    auto F = DIDep->getFlags() | trait::Dependence::UnknownCause;
    if (F & trait::Dependence::May)
      F &= ~trait::Dependence::May;
    DITrait.template set<Tag>(new trait::DIDependence(F));
  }
}

/// Update traits `DIMTrait` according a specified traits `FromDIMTrait`.
///
/// Note, that `FromDIMTrait` may be changed according to traits in `DIMTrait`
/// which should not be updated. Attached representation of traits would not
/// be changed if appropriate trait tag will not be removed.
/// \return `true` if something has been updated.
template<class TraitT>
bool clarifyDescriptor(TraitT &&FromDIMTrait, DIMemoryTrait &DIMTrait) {
  if (DIMTrait.is<trait::NoPromotedScalar>())
    FromDIMTrait.template set<trait::NoPromotedScalar>();
  if (!DIMTrait.is<trait::HeaderAccess>())
    FromDIMTrait.template unset<trait::HeaderAccess>();
  if (DIMTrait.is<trait::AddressAccess>())
    FromDIMTrait.template set<trait::AddressAccess>();
  if (DIMTrait.is<trait::UseAfterLoop>())
    FromDIMTrait.template set<trait::UseAfterLoop>();
  if (DIMTrait.is<trait::DirectAccess>())
    FromDIMTrait.template set<trait::DirectAccess>();
  if (DIMTrait.is<trait::IndirectAccess>())
    FromDIMTrait.template set<trait::IndirectAccess>();
  if (DIMTrait.is<trait::ExplicitAccess>())
    FromDIMTrait.template set<trait::ExplicitAccess>();
  if (!DIMTrait.is<trait::Flow>())
    FromDIMTrait.template unset<trait::Flow>();
  if (!DIMTrait.is<trait::Anti>())
    FromDIMTrait.template unset<trait::Anti>();
  if (!DIMTrait.is<trait::Output>())
    FromDIMTrait.template unset<trait::Output>();
  if (DIMTrait.is_any<trait::NoAccess, trait::Readonly>())
    return false;
  if (DIMTrait.is<trait::Shared>() &&
      !FromDIMTrait.template is_any<trait::NoAccess, trait::Readonly>())
    return false;
  if (DIMTrait.is_any<trait::Private, trait::Induction, trait::Reduction>() &&
      !FromDIMTrait.template is_any<trait::NoAccess, trait::Readonly,
                                    trait::Shared>())
    return false;
  // Do not change 'second to last private' to 'last private'. This occurs
  // after loop rotate.
  if (DIMTrait.is<trait::SecondToLastPrivate>() &&
      FromDIMTrait.template is<trait::LastPrivate>()) {
    if (FromDIMTrait.template is<trait::Shared>() &&
        !DIMTrait.is<trait::Shared>()) {
      DIMTrait.set<trait::Shared>();
      return true;
    }
    return false;
  }
  bool IsChanged = false;
  if (!FromDIMTrait.template is<trait::FirstPrivate>() &&
      DIMTrait.is<trait::FirstPrivate>()) {
    LLVM_DEBUG(dbgs() << "[DA DI]: drop first private\n");
    DIMTrait.unset<trait::FirstPrivate>();
    IsChanged = true;
  }
  if (DIMTrait.is_any<trait::SecondToLastPrivate, trait::LastPrivate>() &&
      !FromDIMTrait.template is_any<trait::NoAccess, trait::Readonly,
                                    trait::Shared, trait::Private>())
    return IsChanged;
  if (DIMTrait.is_any<trait::DynamicPrivate>() &&
      !FromDIMTrait.template is_any<
          trait::NoAccess, trait::Readonly, trait::Shared, trait::Private,
          trait::SecondToLastPrivate, trait::LastPrivate>())
    return IsChanged;
  // Do not use `operator=` because attached values should not be lost.
  bcl::trait::update(FromDIMTrait, DIMTrait);
  LLVM_DEBUG(dbgs() << "[DA DI]: update existing trait to ";
             DIMTrait.print(dbgs()); dbgs() << "\n");

  return true;
}

/// Update traits `DIMTrait` according a specified traits `FromDIMTrait`.
///
/// Note, that `FromDIMTrait` may be invalidated to avoid redundant copy
/// operations.
void clarify(DIMemoryTrait &FromDIMTrait, DIMemoryTrait &DIMTrait) {
  if (!clarifyDescriptor(FromDIMTrait, DIMTrait))
    return;
  moveIf<trait::Flow>(FromDIMTrait, DIMTrait);
  moveIf<trait::Anti>(FromDIMTrait, DIMTrait);
  moveIf<trait::Output>(FromDIMTrait, DIMTrait);
}

/// Update traits for a specified memory `M` in a specified pool. Add new
/// trait if `M` is not stored in pool.
///
/// If traits for `M` already exist in a pool and should not be updated
/// (for example, should be locked) the second returned value is `false`.
/// If existing trait is already accurate it will not be updated.
std::pair<DIMemoryTraitRegionPool::iterator, bool>
addOrUpdateInPool(DIMemory &M, MemoryDescriptor Dptr,
    ArrayRef<const DIMemory *> LockedTraits,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    DIMemoryTraitRegionPool &Pool) {
  auto DIMTraitItr = Pool.find_as(&M);
  if (DIMTraitItr == Pool.end()) {
    LLVM_DEBUG(dbgs() << "[DA DI]: add new trait to pool\n");
    DIMTraitItr = Pool.try_emplace({ &M, &Pool }, Dptr).first;
    return std::make_pair(DIMTraitItr, true);
  }
  bool IsChanged = !isLockedTrait(*DIMTraitItr, LockedTraits, DIAliasSTR) &&
    clarifyDescriptor(Dptr, *DIMTraitItr);
  return std::make_pair(DIMTraitItr, !IsChanged);
}

/// Update traits `DIMTrait` according to alias traits related to a value V
/// which is binded to a specified memory `M`.
///
/// \return `true` if traits have been updated..
bool combineWith(Value *V, DIMemory &M, const AliasTree &AT,
  const DependenceSet::iterator &ATraitItr, DIMemoryTrait &DIMTrait) {
  assert(V && !isa<UndefValue>(V) && "Metadata-level alias tree is corrupted!");
  if (isa<DIUnknownMemory>(M) && cast<DIUnknownMemory>(M).isExec()) {
    auto MTraitItr = ATraitItr->find(cast<Instruction>(V));
    // If memory location is not explicitly accessed in the region and if it
    // does not cover any explicitly accessed location then go to the next
    // location.
    if (MTraitItr == ATraitItr->unknown_end())
      return false;
    auto CombinedTrait = BitMemoryTrait(DIMTrait);
    CombinedTrait &= *MTraitItr;
    // Do not use `operator=` because already attached values should not be lost.
    bcl::trait::update(CombinedTrait.toDescriptor(1, NumTraits), DIMTrait);
    if (DIMTrait.is<trait::NoRedundant>())
      DIMTrait.unset<trait::Redundant>();
    if (DIMTrait.is<trait::DirectAccess>())
      DIMTrait.unset<trait::IndirectAccess>();
    LLVM_DEBUG(dbgs() << "[DA DI]: combine traits to ";
               DIMTrait.print(dbgs()); dbgs() << "\n");
    return true;
  }
  AliasTrait::iterator MTraitItr = ATraitItr->end();
  if (auto *DIEM = dyn_cast<DIEstimateMemory>(&M)) {
    auto EM = AT.find(MemoryLocation(V, DIEM->getSize()));
    // For example, alias tree may not contain global variable if it become
    // unused after transformation.
    assert((M.isOriginal() || EM) &&
      "Estimate memory must be presented in the alias tree!");
    if (!EM)
      return false;
    MTraitItr = ATraitItr->find(EM);
    if (M.isOriginal()) {
      EM = EM->getParent();
      while (MTraitItr == ATraitItr->end() && EM) {
        MTraitItr = ATraitItr->find(EM);
        EM = EM->getParent();
      }
    }
  } else {
    auto EM = AT.find(MemoryLocation(V, 0));
    assert((M.isOriginal() || EM) &&
      "Estimate memory must be presented in the alias tree!");
    if (!EM)
      return false;
    // We do not known size of DIMemory, so bounded node (in alias tree) related
    // to it must contain all possible memory in chain. ATraitItr represents
    // traits for memory in this bounded node, so it could not contain memory
    // smaller than the last node in chain.
    using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
    while (auto Next = CT::getNext(EM))
      EM = Next;
    MTraitItr = ATraitItr->find(EM);
    EM = EM->getParent();
    while (MTraitItr == ATraitItr->end() && EM) {
      MTraitItr = ATraitItr->find(EM);
      EM = EM->getParent();
    }
  }
  // If memory location is not explicitly accessed in the region and if it
  // does not cover any explicitly accessed location then go to the next
  // location.
  if (MTraitItr == ATraitItr->end())
    return false;
  auto HasSpuriousDep = hasSpuriousDep(*MTraitItr) || hasSpuriousDep(DIMTrait);
  bcl::TraitDescriptor<trait::Flow, trait::Anti, trait::Output> DepDptr;
  if (!HasSpuriousDep) {
    bcl::trait::set(DIMTrait, DepDptr);
    bcl::trait::set(*MTraitItr, DepDptr);
  }
  // Do not move combineIf() after update() of DIMTrait because initial value
  // of DIMTrait is necessary to accurately combine attached values.
  combineIf<trait::Flow>(*MTraitItr, DIMTrait);
  combineIf<trait::Anti>(*MTraitItr, DIMTrait);
  combineIf<trait::Output>(*MTraitItr, DIMTrait);
  auto CombinedTrait = BitMemoryTrait(DIMTrait);
  CombinedTrait &= *MTraitItr;
  // Do not use `operator=` because already attached values should not be lost.
  bcl::trait::update(CombinedTrait.toDescriptor(1, NumTraits), DIMTrait);
  // Do not set dependence if it have not been set in any descriptor.
  if (!HasSpuriousDep)
    bcl::trait::update(DepDptr, DIMTrait);
  if (DIMTrait.is<trait::NoRedundant>())
    DIMTrait.unset<trait::Redundant>();
  if (DIMTrait.is<trait::DirectAccess>())
    DIMTrait.unset<trait::IndirectAccess>();
  LLVM_DEBUG(dbgs() << "[DA DI]: combine traits with IR-level traits ";
             MTraitItr->print(dbgs()); dbgs() << "\n");
  LLVM_DEBUG(dbgs() << "[DA DI]: combine traits to ";
             DIMTrait.print(dbgs()); dbgs() << "\n");
  return true;
}

/// Find IR-level memory node which relates to a specified metadata-level node.
///
/// Some IR-level estimate memory can be converted to metadata-level
/// unknown memory. So, metadata-level unknown memory may correspond to
/// adjacent unknown and estimate IR-level alias nodes at the same time.
AliasNode * findBoundAliasNode(AliasTree &AT,
    const SpanningTreeRelation<AliasTree *> &AliasSTR,
    DIAliasUnknownNode &DIN) {
  SmallPtrSet<AliasNode *, 4> BoundNodes;
  for (auto &M : DIN) {
    if (M.emptyBinding())
      continue;
    findBoundAliasNodes(M, AT, BoundNodes);
    assert((M.isOriginal() || !BoundNodes.empty()) &&
      "Metadata-level alias tree is corrupted!");
  }
  if (BoundNodes.empty())
    return nullptr;
  using NodeItr = bcl::IteratorDataAdaptor<
    SmallPtrSetImpl<AliasNode *>::iterator,
    AliasTree *, GraphTraits<Inverse<AliasTree *>>::NodeRef>;
  AliasNode *AN = *findLCA(AliasSTR,
    NodeItr{ BoundNodes.begin(), &AT }, NodeItr{ BoundNodes.end(), &AT });
  auto InBoundNodesItr = std::find_if(AN->child_begin(), AN->child_end(),
      [&AliasSTR, &BoundNodes](AliasNode &N) {
    return &N == *BoundNodes.begin() ||
      AliasSTR.isAncestor(const_cast<AliasNode *>(&N), *BoundNodes.begin());
  });
  if (InBoundNodesItr == AN->child_end())
    return AN;
  for (auto *N: BoundNodes)
    if (N != &*InBoundNodesItr && !AliasSTR.isAncestor(&*InBoundNodesItr, N))
      return AN;
  return &*InBoundNodesItr;
}

/// Find IR-level memory node which relates to a specified metadata-level node.
AliasNode * findBoundAliasNode(AliasTree &AT, DIAliasEstimateNode &DIN) {
  SmallPtrSet<AliasNode *, 1> BoundNodes;
  for (auto &M : DIN) {
    if (M.emptyBinding())
      continue;
    findBoundAliasNodes(M, AT, BoundNodes);
    assert(BoundNodes.size() == 1 && "Metadata-level alias tree is corrupted!");
#ifndef LLVM_DEBUG
    return *BoundNodes.begin();
  }
  return nullptr;
#else
  }
  assert((BoundNodes.empty() || BoundNodes.size() == 1) &&
    "Metadata-level alias tree is corrupted!");
  return BoundNodes.empty() ? nullptr : *BoundNodes.begin();
#endif
}

AliasNode * findBoundAliasNode(AliasTree &AT,
    const SpanningTreeRelation<AliasTree *> &AliasSTR, DIAliasMemoryNode &DIN) {
  if (isa<DIAliasEstimateNode>(DIN))
    return findBoundAliasNode(AT, cast<DIAliasEstimateNode>(DIN));
  return findBoundAliasNode(AT, AliasSTR, cast<DIAliasUnknownNode>(DIN));
}


/// Convert IR-level reduction kind to metadata-level reduction kind.
///
/// \pre RD represents a valid reduction, RK_NoReduction kind is not permitted.
trait::Reduction::Kind getReductionKind(
  const RecurrenceDescriptor &RD) {
  switch (const_cast<RecurrenceDescriptor &>(RD).getRecurrenceKind()) {
  case RecurKind::Add:
  case RecurKind::FAdd:
  case RecurKind::FMulAdd:
    return trait::DIReduction::RK_Add;
  case RecurKind::Mul:
  case RecurKind::FMul:
    return trait::DIReduction::RK_Mult;
  case RecurKind::Or:
    return trait::DIReduction::RK_Or;
  case RecurKind::And:
    return trait::DIReduction::RK_And;
  case RecurKind::Xor:
    return trait::DIReduction::RK_Xor;
  case RecurKind::FMax:
  case RecurKind::SMax:
  case RecurKind::UMax:
    return trait::DIReduction::RK_Max;
  case RecurKind::FMin:
  case RecurKind::SMin:
  case RecurKind::UMin:
    return trait::DIReduction::RK_Min;
  case RecurKind::SelectICmp:
  case RecurKind::SelectFCmp:
    //TODO (kaniandr@gmail.com): add support for these kinds of reductions.
    return trait::DIReduction::RK_NoReduction;
  }
  llvm_unreachable("Unknown kind of reduction!");
  return trait::DIReduction::RK_NoReduction;
}

bool handleLoopEmptyBindings(
    const Loop *L, const DIMemoryTrait &DITraitItr,
    const DIMemoryTraitRegionPool &Pool,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR) {
  auto *DIM = DITraitItr.getMemory();
  if (DIM->emptyBinding())
    return true;
  if (!DITraitItr.is<trait::DirectAccess>())
    return false;
  for (auto &Ptr : *DIM) {
    if (!Ptr.pointsToAliveValue())
      continue;
    if (isa<Instruction>(Ptr) && L->contains(cast<Instruction>(Ptr)))
      return false;
    if (any_of_user_insts(*Ptr, [L](auto *U) {
          return isa<Instruction>(U) && L->contains(cast<Instruction>(U));
        }))
      return false;
  }
  for (auto &T : Pool) {
    if (!T.is<trait::DirectAccess>())
      continue;
    auto *PoolNode = T.getMemory()->getAliasNode();
    auto *MemNode = DIM->getAliasNode();
    if (T.getMemory() != DIM && !DIAliasSTR.isUnreachable(PoolNode, MemNode))
      return false;
  }
  return true;
}

MDNode *findInAliasTreeMapping(const Function *F, const DIMemoryLocation &Loc) {
  auto MD = F->getMetadata("alias.tree.mapping");
  if (MD == nullptr)
    return nullptr;
  for (auto &op : MD->operands()) {
    auto *DIN = dyn_cast<MDNode>(op);
    if (!DIN)
      continue;
    assert(DIN->getNumOperands() == 2 &&
           "Alias tree mapping node must contain two operands!");
    auto *DINewVar = dyn_cast<DIVariable>(DIN->getOperand(1));
    if (Loc.Var == DINewVar)
      return dyn_cast<MDNode>(DIN->getOperand(0));
  }
  return nullptr;
}

/// Update traits of metadata-level locations related to a specified Phi-node
/// in a specified loop. This function uses a specified `TraitInserter` functor
/// to update traits for a single memory location.
template<class FuncT> void updateTraits(const Loop *L, const PHINode *Phi,
    const DominatorTree &DT, Optional<unsigned> DWLang,
    ArrayRef<const DIMemory *> LockedTraits,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    DIMemoryTraitRegionPool &Pool,
    ArrayRef<Instruction *> MDSearchIterationUsers, FuncT &&TraitInserter) {
  SmallVector<DIMemoryLocation, 2> DILocs;
  for (const auto &Incoming : Phi->incoming_values()) {
    if (!L->contains(Phi->getIncomingBlock(Incoming)))
      continue;
    LLVM_DEBUG(dbgs() << "[DA DI]: traits for promoted location ";
      printLocationSource(dbgs(), Incoming, &DT); dbgs() << " found \n");
    Instruction * Users[] = { &Phi->getIncomingBlock(Incoming)->back() };
    findMetadata(Incoming, Users, DT, DILocs);
  }
  if (!MDSearchIterationUsers.empty())
    findMetadata(Phi, MDSearchIterationUsers, DT, DILocs);
  if (DILocs.empty())
    return;
  for (auto &DILoc : DILocs) {
    auto *MD = findInAliasTreeMapping(Phi->getFunction(), DILoc);
    if (!MD)
      MD = getRawDIMemoryIfExists(Phi->getContext(), DILoc);
    // If a memory location is partially promoted we will try to use
    // dbg.declare or dbg.addr intrinsics to find the corresponding node in
    // the alias tree.
    if (!MD) {
      if (DILoc.Expr->getNumOperands() != 0)
        continue;
      auto *MDV =
          MetadataAsValue::getIfExists(Phi->getContext(), DILoc.Var);
      if (!MDV)
        continue;
      for (User *U : MDV->users()) {
        if (auto *DII = dyn_cast<DbgVariableIntrinsic>(U))
          if (DII->isAddressOfVariable()) {
            auto DILocCandidate = DIMemoryLocation::get(DII);
            if (DILocCandidate.Expr != DILoc.Expr &&
                DILocCandidate.Loc == DILoc.Loc)
              continue;
            MD = getRawDIMemoryIfExists(Phi->getContext(), DILocCandidate);
            if (MD)
              break;
          }
      }
      if (!MD)
        continue;
    }
    auto DIMTraitItr = Pool.find_as(MD);
    if (DIMTraitItr == Pool.end() ||
        DIMTraitItr->getMemory()->isOriginal() ||
        !handleLoopEmptyBindings(L, *DIMTraitItr, Pool, DIAliasSTR) ||
        !DIMTraitItr->is<trait::Anti, trait::Flow, trait::Output>() ||
        isLockedTrait(*DIMTraitItr, LockedTraits, DIAliasSTR))
      continue;
    LLVM_DEBUG(if (DWLang) {
      dbgs() << "[DA DI]: update traits for ";
      printDILocationSource(*DWLang, *DIMTraitItr->getMemory(), dbgs());
      dbgs() << "\n";
    });
    TraitInserter(*DIMTraitItr);
  }
}

/// Combine traits for memory locations and set traits for the whole alias node.
void combineTraits(bool IgnoreRedundant, DIAliasTrait &DIATrait) {
  assert(!DIATrait.empty() && "List of traits must not be empty!");
  if (DIATrait.size() == 1) {
    auto DIMTraitItr = *DIATrait.begin();
    DIATrait = *DIMTraitItr;
    if (!(DIMTraitItr->is<trait::ExplicitAccess>() &&
         !DIMTraitItr->is<trait::NoAccess>() &&
         DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode()))
      DIATrait.unset<trait::ExplicitAccess>();
    assert(!(DIMTraitItr->is<trait::Redundant>() &&
             DIMTraitItr->is<trait::NoRedundant>()) &&
      "Conflict in traits for a memory location!");
    assert((DIMTraitItr->is<trait::Redundant>() ||
           DIMTraitItr->is<trait::NoRedundant>()) &&
      "One of traits must be set!");
    if (!(DIMTraitItr->is<trait::Redundant>() &&
          DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode()))
      DIATrait.unset<trait::Redundant>();
    if (!(DIMTraitItr->is<trait::NoRedundant>() &&
          DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode()))
      DIATrait.unset<trait::NoRedundant>();
    if (IgnoreRedundant && DIMTraitItr->is<trait::Redundant>()) {
      DIATrait.set<trait::NoAccess>();
      DIATrait.unset<trait::HeaderAccess, trait::AddressAccess,
                     trait::UseAfterLoop>();
    }
    if (!(DIMTraitItr->is<trait::NoPromotedScalar>() &&
          DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode()))
      DIATrait.unset<trait::NoPromotedScalar>();
    if (!(DIMTraitItr->is<trait::DirectAccess>() &&
          DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode()))
      DIATrait.unset<trait::DirectAccess>();
    if (!(DIMTraitItr->is<trait::IndirectAccess>() &&
          DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode()))
      DIATrait.unset<trait::IndirectAccess>();
    LLVM_DEBUG(dbgs() << "[DA DI]: set combined trait for single location to ";
      DIATrait.print(dbgs()); dbgs() << "\n");
    return;
  }
  BitMemoryTrait CombinedTrait;
  bool ExplicitAccess = false, Redundant = false, NoRedundant = false;
  bool NoPromotedScalar = false, DirectAccess = false, IndirectAccess = false;
  unsigned NumberOfCombined = 0;
  bcl::TraitDescriptor<trait::Flow, trait::Anti, trait::Output> DepDptr;
  bool HasSpuriousDep = false;
  for (auto &DIMTraitItr : DIATrait) {
    if (DIMTraitItr->is<trait::ExplicitAccess>() &&
        !DIMTraitItr->is<trait::NoAccess>() &&
        DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode())
      ExplicitAccess = true;
    if (DIMTraitItr->is<trait::NoPromotedScalar>() &&
        DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode())
      NoPromotedScalar = true;
    if (DIMTraitItr->is<trait::DirectAccess>() &&
        DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode())
      DirectAccess = true;
    if (DIMTraitItr->is<trait::IndirectAccess>() &&
        DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode())
      IndirectAccess = true;
    assert(!(DIMTraitItr->is<trait::Redundant>() &&
             DIMTraitItr->is<trait::NoRedundant>()) &&
      "Conflict in traits for a memory location!");
    assert((DIMTraitItr->is<trait::Redundant>() ||
           DIMTraitItr->is<trait::NoRedundant>()) &&
      "One of traits must be set!");
    if (DIMTraitItr->is<trait::Redundant>()) {
      if (DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode()) {
        Redundant = true;
        CombinedTrait &= BitMemoryTrait::Redundant;
      }
      if (IgnoreRedundant)
        continue;
    } else {
      assert(DIMTraitItr->is<trait::NoRedundant>() && "Trait must be set!");
      if (DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode())
        NoRedundant = true;
    }
    if (DIMTraitItr->is<trait::IndirectAccess>() &&
        !DIMTraitItr->is<trait::DirectAccess>()) {
      bool Ignore = true;
      for (auto MH : *DIMTraitItr->get<trait::IndirectAccess>()) {
        assert(MH && "Memory must not be null!");
        auto CoverItr = DIATrait.find(MH);
        if (CoverItr == DIATrait.end() ||
            IgnoreRedundant && (**CoverItr).is<trait::Redundant>()) {
          Ignore = false;
          break;
        }
      }
      if (Ignore)
        continue;
    }
    if (!DIMTraitItr->is<trait::NoAccess>())
      ++NumberOfCombined;
    CombinedTrait &= *DIMTraitItr;
    HasSpuriousDep |= hasSpuriousDep(*DIMTraitItr);
    bcl::trait::set(*DIMTraitItr, DepDptr);
  }
  // It may be 1, if some traits have been ignored (for example, redundant).
  // It may be 0, if there are no memory accesses (for example,
  // address accesses only).
  if (NumberOfCombined > 1) {
    auto OriginalTrait = CombinedTrait;
    CombinedTrait |= BitMemoryTrait::AllUnitFlags;
    CombinedTrait &=
      dropUnitFlag(OriginalTrait) == BitMemoryTrait::NoAccess ?
        BitMemoryTrait::NoAccess :
      dropUnitFlag(OriginalTrait) == BitMemoryTrait::Readonly ?
        BitMemoryTrait::Readonly :
      hasSharedJoin(OriginalTrait) ? BitMemoryTrait::Shared :
        BitMemoryTrait::Dependency;
  }
  auto Dptr = CombinedTrait.toDescriptor(0, NumTraits);
  if (!HasSpuriousDep)
    bcl::trait::update(DepDptr, Dptr);
  if (!ExplicitAccess)
    Dptr.unset<trait::ExplicitAccess>();
  if (!NoPromotedScalar)
    Dptr.unset<trait::NoPromotedScalar>();
  if (!Redundant)
    Dptr.unset<trait::Redundant>();
  if (!NoRedundant)
    Dptr.unset<trait::NoRedundant>();
  if (!DirectAccess)
    Dptr.unset<trait::DirectAccess>();
  if (!IndirectAccess)
    Dptr.unset<trait::IndirectAccess>();
  DIATrait = Dptr;
  LLVM_DEBUG(dbgs() << "[DA DI]: set combined trait to ";
    Dptr.print(dbgs()); dbgs() << "\n");
}

/// Representation of a IR-level memory locations binded to a specified one.
template<class BindRef> struct BindingT {
  using BindRefT = BindRef;
  explicit BindingT(const DIMemory *M) : Memory(M) {}
  const DIMemory * Memory;
  SmallVector<BindRef, 4> BindedMemory;
};

/// Tag for TraitsSanitizer (see TraitsSanitizer::EstimateCoverageT for example).
struct LastSwapOffset {};

/// Check relation between memory locations and update traits for location
/// which is covered by some other location with the same binded IR-level
/// memory representation.
///
/// Use CRTP do determine sanitizeImpl() and initializeImpl() methods.
template<class Impl> class TraitsSanitizer {
protected:
  /// Estimate memory coverage.
  ///
  /// Locations from these containers should be checked to determine whether
  /// some other location is covered.
  /// The key in the map is an IR-level estimate memory location binded to
  /// a list of metadata-level locations from a value. The second argument of
  /// a value (in tuple) is utility value which is used to sort locations during
  /// search. Note, to avoid out of range swaps the size of list is greater than
  /// a number of locations it contains and the first element is a poison value
  /// which should not be dereferenced.
  using EstimateCoverageT = PersistentMap<const EstimateMemory *,
    std::tuple<SmallVector<DIMemory *, 1>, unsigned>,
    DenseMapInfo<const EstimateMemory *>,
    TaggedDenseMapTuple<
      bcl::tagged<const EstimateMemory *, EstimateMemory>,
      bcl::tagged<SmallVector<DIMemory *, 1>, DIMemory>,
      bcl::tagged<unsigned, LastSwapOffset>>>;

  /// Unknown memory coverage.
  using UnknownCoverageT = PersistentMap<const Value *,
    std::tuple<SmallVector<DIMemory *, 1>, unsigned>,
    DenseMapInfo<const Value *>,
    TaggedDenseMapTuple<
      bcl::tagged<const Value *, Value>,
      bcl::tagged<SmallVector<DIMemory *, 1>, DIMemory>,
      bcl::tagged<unsigned, LastSwapOffset>>>;
public:
  TraitsSanitizer(const AliasTree &AT, Optional<unsigned> DWLang) :
    mAT(AT), mDWLang(DWLang) {}

  /// Sanitize traits.
  void exec() {
    initialize();
    sanitizeTraits();
  }

  /// Should be implemented in derived class.
  template<class CoverageItrT>
  void sanitizeImpl(const DIMemory *, CoverageItrT, CoverageItrT) {}

  /// Should be implemented in derived class.
  void initializeImpl() {}

private:
  template<class CoverageItrT>
  void sanitize(const DIMemory *M, CoverageItrT BeginItr, CoverageItrT EndItr) {
    static_cast<Impl *>(this)->sanitizeImpl(M, BeginItr, EndItr);
  }

  void initialize() { static_cast<Impl *>(this)->initializeImpl(); }

  /// Check that there is a location in estimate coverage which covers a
  /// specified one. These two locations must be from the same estimate memory
  /// tree.
  EstimateCoverageT::iterator isCovered(const EstimateMemory *EM) {
    while (EM) {
      auto Itr = mEstimateCoverage.find(EM);
      if (Itr != mEstimateCoverage.end())
        return Itr;
      EM = EM->getParent();
    }
    return mEstimateCoverage.end();
  };

  /// Return true if there is metadata-level memory location which differs from
  /// a specified one and covers IR-level estimate memory location binded to
  /// a specified location `M`.
  ///
  /// Use this after sort() only.
  EstimateCoverageT::persistent_iterator isCovered(const DIMemory *M,
      const EstimateCoverageT::persistent_iterator &EMCoverageItr) {
    // If location is sorted it is safe to check first valid location only.
    // Note, that the front() location is poison and should not be accessed.
    if (EMCoverageItr->template get<DIMemory>()[1] != M)
      return EMCoverageItr;
    auto Itr = isCovered(EMCoverageItr->get<EstimateMemory>()->getParent());
    return Itr != mEstimateCoverage.end() &&
      Itr->template get<DIMemory>()[1] != M ? Itr :
      EstimateCoverageT::persistent_iterator();
  }

  /// Return true if there is metadata-level memory location which differs from
  /// a specified one and covers IR-level unknown memory location binded to
  /// a specified location `M`.
  ///
  /// Use this after sort() only.
  UnknownCoverageT::persistent_iterator isCovered(const DIMemory *M,
      const UnknownCoverageT::persistent_iterator &UMCoverageItr) {
    // If location is sorted it is safe to check first valid location only.
    // Note, that the front() location is poison and should not be accessed.
    return UMCoverageItr->get<DIMemory>()[1] != M ? UMCoverageItr :
      UnknownCoverageT::persistent_iterator();
  }

  /// Return true if there are metadata-level locations which covers a specified
  /// one.
  ///
  /// Note, that found and a specified locations may be equal.
  /// So, for the safety, call this method for a location if it is not stored
  /// in coverage containers.
  bool isCovered(const DIMemory &M, SmallPtrSetImpl<DIMemory *> &Coverage) {
    if (isa<DIUnknownMemory>(M) && cast<DIUnknownMemory>(M).isExec()) {
      SmallVector<UnknownCoverageT::iterator, 4> ItrCoverage;
      for (auto &VH : M) {
        auto Itr = mUnknownCoverage.find(VH);
        if (Itr == mUnknownCoverage.end())
          return false;
        ItrCoverage.push_back(Itr);
      }
      for (auto &I : ItrCoverage)
        Coverage.insert(I->get<DIMemory>().begin() + 1,
          I->get<DIMemory>().end());
    } else {
      SmallVector<EstimateCoverageT::iterator, 4> ItrCoverage;
      auto Size = isa<DIEstimateMemory>(M) ?
        cast<DIEstimateMemory>(M).getSize() : 0;
      for (auto &VH : M) {
        auto *EM = mAT.find(MemoryLocation(VH, Size));
        auto Itr = isCovered(EM);
        if (Itr == mEstimateCoverage.end())
          return false;
        ItrCoverage.push_back(Itr);
      }
      for (auto &I : ItrCoverage)
        Coverage.insert(I->get<DIMemory>().begin() + 1,
          I->get<DIMemory>().end());
    }
    return true;
  }

  void sanitizeTraits() {
    for (auto *M : mAccesses) {
      SmallPtrSet<DIMemory *, 4> Coverage;
      if (isCovered(*M, Coverage)) {
        LLVM_DEBUG(if (mDWLang) {
          dbgs() << "[DA DI]: sanitize covered access ";
          printDILocationSource(*mDWLang, *M, dbgs());
          dbgs() << "\n";
        });
        sanitize(M, Coverage.begin(), Coverage.end());
      }
    }
    sanitizeCorrupted(mCorruptedEMs.rbegin(), mCorruptedEMs.rend());
    sanitizeCorrupted(mCorruptedUMs.rbegin(), mCorruptedUMs.rend());
  }

  template<class ItrT> void resetLastSwapOffset(
    const ItrT &BeginItr, const ItrT &EndItr) {
    for (auto &Binding : make_range(BeginItr, EndItr))
      for (auto &BindItr : Binding.BindedMemory)
        BindItr->template get<LastSwapOffset>() = 0;
  }

  /// Sort corrupted locations from a specified range.
  ///
  /// For example, BeginItr points to a location from mCorruptedEMs (it also
  /// may be from mCorruptedUMs):
  /// {
  ///   M, {
  ///     EM1 -> {M1, M2, M, M3},
  ///     EM2 -> {M4, M}
  ///   }
  /// }
  /// EM1 and EM2 are binded to `M`, they also binded to M1, M2, M3 and M4
  /// correspondingly.
  /// This method move M to the end of the list of memory locations attached
  /// to IR-level memory location in coverage:
  /// {
  ///   M, {
  ///     EM1 -> {M1, M2, M3, M},
  ///     EM2 -> {M4, M}
  ///   }
  /// }
  /// If BeginItr + 1 points to M2:
  /// {
  ///   M2, {
  ///     EM1 -> {M1, M2, M3, M},
  ///     EM3 -> {M2}
  ///   }
  /// }
  /// it will be sorted in the following way
  /// {
  ///   M2, {
  ///     EM1 -> {M1, M3, M2, M},
  ///     EM3 -> {M2}
  ///   }
  /// }
  /// So, M is always the last one location in a list (if list contains it),
  /// M2 is always stored before M (if a list contains both and is the last
  /// one if a list does not contain M, etc.).
  template<class ItrT> void sort(const ItrT &BeginItr, const ItrT &EndItr) {
    resetLastSwapOffset(BeginItr, EndItr);
    for (auto &Binding : make_range(BeginItr, EndItr)) {
      for (auto &BindItr : Binding.BindedMemory) {
        auto MItr = std::find(BindItr->template get<DIMemory>().begin(),
          BindItr->template get<DIMemory>().end(), Binding.Memory);
        std::swap(*MItr,
          BindItr->template get<DIMemory>()[
            BindItr->template get<DIMemory>().size() -
              (++BindItr->template get<LastSwapOffset>())]);
      }
    }
    resetLastSwapOffset(BeginItr, EndItr);
  }

#ifdef LLVM_DEBUG
  template <class ItrT>
  void dumpCandidates(const ItrT &BeginItr, const ItrT &EndItr) {
    for (auto &Binding : make_range(BeginItr, EndItr)) {
      printDILocationSource(*mDWLang, *Binding.Memory, dbgs());
      dbgs() << "\n";
      for (auto &BindItr : Binding.BindedMemory) {
        dbgs() << "  ";
        printLocationSource(dbgs(), *BindItr->first, &mAT.getDomTree());
        dbgs() << " -> ";
        for (auto I = BindItr->template get<DIMemory>().begin() + 1,
             EI = BindItr->template get<DIMemory>().end(); I != EI; ++I) {
          printDILocationSource(*mDWLang, **I, dbgs());
          dbgs() << " ";
        }
        dbgs() << "\n";
      }
    }
  }
#endif

  /// Sanitize corrupted locations from a specified range.
  ///
  /// At first, locations from a specified range will be sorted (see sort()).
  /// Let us consider an example presented in a sort() description. We have
  /// already sorted locations M, M2:
  /// {
  ///   M, {
  ///     EM1 -> {M1, M3, M2, M},
  ///     EM2 -> {M4, M}
  ///   }
  /// }
  /// {
  ///   M2, {
  ///     EM1 -> {M1, M3, M2, M},
  ///     EM3 -> {M2}
  ///   }
  /// }
  /// At first, we check `M` and find that it is covered by M1 (EM1)
  /// and M4 (EM2). So, it can be sanitized. Then we remove at from related
  /// lists because sanitized location should not be used in a coverage for
  /// other locations. We also swap it with (list size - swap offset - 1)
  /// location. This is necessary to preserve the right order of locations
  /// if there were swaps already.
  /// {
  ///   M, {
  ///     EM1 -> {M1, M3, M2},
  ///     EM2 -> {M4}
  ///   }
  /// }
  /// {
  ///   M2, {
  ///     EM1 -> {M1, M3, M2},
  ///     EM3 -> {M2}
  ///   }
  /// }
  /// Now, we check M2. Note, that EM3 can be covered by M2 only. Hence M2
  /// could not be sanitized. So we swap M2 with (list size - swap offset - 2)
  /// location and increment offset (swap should be performed for each list,
  /// swap offset may be different for different lists). Then we increase swap
  /// offset. Note, to avoid out of range swaps the size of list is greater than
  /// a number of locations it contains and the first element is a poison value
  /// which should not be dereferenced.
  /// {
  ///   M, {
  ///     EM1 -> {M1, M2, M3},
  ///     EM2 -> {M4}
  ///   }
  /// }
  /// {
  ///   M2, {
  ///     EM1 -> {M1, M2, M3},
  ///     EM3 -> {M2}
  ///   }
  /// }
  /// Now, we check the next location, for example M3. It is at the end of
  /// all lists, so it can be safely removed using pop_back() if necessary.
  template<class ItrT>
  void sanitizeCorrupted(const ItrT &BeginItr, const ItrT &EndItr) {
    if (BeginItr == EndItr)
      return;
    sort(BeginItr, EndItr);
    LLVM_DEBUG(dbgs() << "[DA DI]: sorted list of covered and corrupted "
                         "candidates:\n"; dumpCandidates(BeginItr, EndItr));
    for (auto &Binding : make_range(BeginItr, EndItr)) {
      bool IsCovered = true;
      SmallVector<typename std::decay<decltype(Binding)>::type::BindRefT, 4>
          ItrCoverage;
      for (auto &BindItr : Binding.BindedMemory) {
        auto Itr = isCovered(Binding.Memory, BindItr);
        if (!Itr) {
          IsCovered = false;
          break;
        }
        ItrCoverage.push_back(std::move(Itr));
      }
      if (IsCovered) {
        LLVM_DEBUG(if (mDWLang) {
          dbgs() << "[DA DI]: sanitize covered corrupted ";
          printDILocationSource(*mDWLang, *Binding.Memory, dbgs());
          dbgs() << "\n";
        });
        SmallPtrSet<DIMemory *, 4> Coverage;
        for (auto &I : ItrCoverage) {
          auto CopyBeginItr = I->template get<DIMemory>().begin() + 1;
          auto CopyEndItr = I->template get<DIMemory>().end();
          if (I->template get<DIMemory>().back() == Binding.Memory) {
            --CopyEndItr;
          } else if (!I->template get<DIMemory>().back()) {
            // The last possible swap operation relates to the first element
            // which is a poison value and the last element. So, the last
            // element becomes poison and the first one becomes valid.
            --CopyBeginItr;
            --CopyEndItr;
          }
          Coverage.insert(CopyBeginItr, CopyEndItr);
        }
        sanitize(Binding.Memory, Coverage.begin(), Coverage.end());
        for (auto &BindItr : Binding.BindedMemory) {
          assert(BindItr->template get<DIMemory>().size() >
            BindItr->template get<LastSwapOffset>() && "Too much swaps!");
          assert(BindItr->template get<DIMemory>().back() == Binding.Memory &&
            "Invariant broken!");
          BindItr->template get<DIMemory>().pop_back();
          std::swap(BindItr->template get<DIMemory>().back(),
            BindItr->template get<DIMemory>()[
              BindItr->template get<DIMemory>().size() -
                BindItr->template get<LastSwapOffset>() - 1]);
        }
      } else {
        for (auto &BindItr: Binding.BindedMemory) {
          assert(BindItr->template get<DIMemory>().back() == Binding.Memory &&
            "Invariant broken!");
          assert(BindItr->template get<DIMemory>().size() >
            BindItr->template get<LastSwapOffset>() + 1 && "Too much swaps!");
          std::swap(BindItr->template get<DIMemory>().back(),
            BindItr->template get<DIMemory>()[
              BindItr->template get<DIMemory>().size() -
                (++BindItr->template get<LastSwapOffset>()) - 1]);
        }
      }
    }
  }

protected:
  Optional<unsigned> mDWLang;
  const AliasTree &mAT;

  EstimateCoverageT::iterator getEstimateCoverage(const EstimateMemory *EM) {
    auto Info = mEstimateCoverage.try_emplace(EM);
    if (Info.second)
      Info.first->template get<DIMemory>().push_back(nullptr);
    return Info.first;
  }

  UnknownCoverageT::iterator getUnknownCoverage(const Value *V) {
    auto Info = mUnknownCoverage.try_emplace(V);
    if (Info.second)
      Info.first->template get<DIMemory>().push_back(nullptr);
    return Info.first;
  }

  /// Locations which should be check and which should not be presented in
  /// coverage (this is precondition).
  std::vector<const DIMemory *> mAccesses;
  /// Corrupted locations which are presented in coverage but also should
  /// be checked.
  std::vector<BindingT<EstimateCoverageT::persistent_iterator>> mCorruptedEMs;
  /// Corrupted locations which are presented in coverage but also should
  /// be checked.
  std::vector<BindingT<UnknownCoverageT::persistent_iterator>> mCorruptedUMs;

private:
  EstimateCoverageT mEstimateCoverage;
  UnknownCoverageT mUnknownCoverage;
};

/// Determine redundant corrupted locations (if no redundant cover some other
/// location than this covered location is redundant).
class RedundantSearch : public TraitsSanitizer<RedundantSearch> {
public:
  RedundantSearch(const AliasTree &AT, Optional<unsigned> DWLang,
      DIAliasTrait &DIATrait, ArrayRef<const DIMemory *> LockedTraits,
      const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
      const SmallPtrSetImpl<const Value *> &NoAccessValues) :
    TraitsSanitizer<RedundantSearch>(AT, DWLang),
    mDIATrait(DIATrait), mLockedTraits(LockedTraits), mDIAliasSTR(DIAliasSTR),
    mNoAccessValues(NoAccessValues) {}

  template<class CoverageItrT>
  void sanitizeImpl(const DIMemory *M, CoverageItrT I, CoverageItrT EI) {
    (**mDIATrait.find(M)).set<trait::Redundant>(new trait::DICoverage(I, EI));
    (**mDIATrait.find(M)).unset<trait::NoRedundant>();
  }

  void initializeImpl() {
    for (auto &DIMTraitItr : mDIATrait) {
      assert(!(DIMTraitItr->is<trait::Redundant>() &&
               DIMTraitItr->is<trait::NoRedundant>()) &&
        "Conflict in traits for a memory location!");
      assert((DIMTraitItr->is<trait::Redundant>() ||
             DIMTraitItr->is<trait::NoRedundant>()) &&
        "One of traits must be set!");
      auto *M = DIMTraitItr->getMemory();
      if (M->emptyBinding() || DIMTraitItr->is<trait::Redundant>() ||
          isLockedTrait(*DIMTraitItr, mLockedTraits, mDIAliasSTR))
        continue;
      if (isa<DIUnknownMemory>(M) && cast<DIUnknownMemory>(M)->isExec()) {
        if (M->isOriginal())
          mCorruptedUMs.emplace_back(M);
        SmallPtrSet<const Value *, 8> Binding;
        for (auto &VH : *M) {
          if (Binding.insert(VH).second) {
            auto Itr = getUnknownCoverage(VH);
            Itr->get<DIMemory>().push_back(const_cast<DIMemory *>(M));
            Itr->get<LastSwapOffset>() = 0;
            if (M->isOriginal())
              mCorruptedUMs.back().BindedMemory.emplace_back(Itr);
          }
        }
      } else {
        if (M->isOriginal())
          mCorruptedEMs.emplace_back(M);
        auto Size = isa<DIEstimateMemory>(M) ?
          cast<DIEstimateMemory>(M)->getSize() : 0;
        SmallPtrSet<const EstimateMemory *, 8> Binding;
        for (auto &VH : *M) {
          if (mNoAccessValues.count(VH))
            continue;
          auto *EM = mAT.find(MemoryLocation(VH, Size));
          if (!EM)
            continue;
          if (isa<DIUnknownMemory>(M)) {
            using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
            while (auto Next = CT::getNext(EM))
              EM = Next;
          }
          if (Binding.insert(EM).second) {
            auto Itr = getEstimateCoverage(EM);
            Itr->get<DIMemory>().push_back(const_cast<DIMemory *>(M));
            Itr->get<LastSwapOffset>() = 0;
            if (M->isOriginal())
              mCorruptedEMs.back().BindedMemory.emplace_back(Itr);
          }
        }
      }
    }
  }

private:
  DIAliasTrait mDIATrait;
  ArrayRef<const DIMemory *> mLockedTraits;
  const SpanningTreeRelation<const tsar::DIAliasTree *> &mDIAliasSTR;
  const SmallPtrSetImpl<const Value *> &mNoAccessValues;
};

/// Perform search for memory which is not directly accessed in a loop and
/// which is covered by some other corrupted location.
///
/// This means that such corrupted location is used instead of a covered
/// location in the original program. Hence, this covered location can be
/// ignored in description of loop traits. Redundant locations will be ignored
/// if option is set.
class IndirectAccessSanitizer :
  public TraitsSanitizer<IndirectAccessSanitizer> {
public:
  IndirectAccessSanitizer(const AliasTree &AT, Optional<unsigned> DWLang,
    DIAliasTrait &DIATrait, bool IgnoreRedundant) :
    TraitsSanitizer<IndirectAccessSanitizer>(AT, DWLang),
    mDIATrait(DIATrait), mIgnoreRedundant(IgnoreRedundant) {}

  template<class CoverageItrT>
  void sanitizeImpl(const DIMemory *M, CoverageItrT I, CoverageItrT EI) {
    (**mDIATrait.find(M)).set<trait::IndirectAccess>(
      new trait::DICoverage(I, EI));
  }

  void initializeImpl() {
    for (auto &DIMTraitItr : mDIATrait) {
      auto *M = DIMTraitItr->getMemory();
      assert(!(DIMTraitItr->is<trait::Redundant>() &&
               DIMTraitItr->is<trait::NoRedundant>()) &&
        "Conflict in traits for a memory location!");
      assert((DIMTraitItr->is<trait::Redundant>() ||
             DIMTraitItr->is<trait::NoRedundant>()) &&
        "One of traits must be set!");
      if (M->emptyBinding() ||
          mIgnoreRedundant && DIMTraitItr->is<trait::Redundant>())
        continue;
      if (!DIMTraitItr->is<trait::DirectAccess>() && !M->isOriginal())
        mAccesses.push_back(M);
      if (!M->isOriginal())
        continue;
      if (isa<DIUnknownMemory>(M) && cast<DIUnknownMemory>(M)->isExec()) {
        if (!DIMTraitItr->is<trait::DirectAccess>())
          mCorruptedUMs.emplace_back(M);
        SmallPtrSet<const Value *, 8> Binding;
        for (auto &VH : *M) {
          if (Binding.insert(VH).second) {
            auto Itr = getUnknownCoverage(VH);
            Itr->get<DIMemory>().push_back(const_cast<DIMemory *>(M));
            Itr->get<LastSwapOffset>() = 0;
            if (!DIMTraitItr->is<trait::DirectAccess>())
              mCorruptedUMs.back().BindedMemory.emplace_back(Itr);
          }
        }
      } else {
        bool NeedCheck = !DIMTraitItr->is<trait::DirectAccess>();
        if (NeedCheck)
          mCorruptedEMs.emplace_back(M);
        auto Size = isa<DIEstimateMemory>(M) ?
          cast<DIEstimateMemory>(M)->getSize() : 0;
        SmallPtrSet<const EstimateMemory *, 8> Binding;
        for (auto &VH : *M) {
          auto *EM = mAT.find(MemoryLocation(VH, Size));
          if (!EM) {
            if (NeedCheck)
              mCorruptedEMs.pop_back();
            NeedCheck = false;
            continue;
          }
          if (isa<DIUnknownMemory>(M)) {
            using CT = bcl::ChainTraits<EstimateMemory, Hierarchy>;
            while (auto Next = CT::getNext(EM))
              EM = Next;
          }
          if (Binding.insert(EM).second) {
            auto Itr = getEstimateCoverage(EM);
            Itr->get<DIMemory>().push_back(const_cast<DIMemory *>(M));
            Itr->get<LastSwapOffset>() = 0;
            if (NeedCheck)
              mCorruptedEMs.back().BindedMemory.emplace_back(Itr);
          }
        }
      }
    }
  }
private:
  DIAliasTrait &mDIATrait;
  bool mIgnoreRedundant;
};
}

void DIDependencyAnalysisPass::analyzePrivatePromoted(Loop *L,
    Optional<unsigned> DWLang,
    const tsar::SpanningTreeRelation<AliasTree *> &AliasSTR,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const DIMemory *> LockedTraits, DIMemoryTraitRegionPool &Pool) {
  auto *Head = L->getHeader();
  // For each instruction we determine variables which it uses. Then we explore
  // instructions which computes values of these variables. We check whether
  // these instructions and currently processes instruction are always executed
  // on the same iteration.
  SmallDenseSet<DIMemoryLocation, 8> OutwardUses, Privates;
  SmallDenseMap<DIMemoryLocation, SmallVector<Instruction *, 8>, 8> OutwardDefs;
  for (auto *BB : L->blocks()) {
    for (auto &I : *BB) {
      if (auto II = dyn_cast<IntrinsicInst>(&I))
        if (isDbgInfoIntrinsic(II->getIntrinsicID()) ||
            isMemoryMarkerIntrinsic(II->getIntrinsicID()))
          continue;
      // Ignore Phi-nodes which just move the value throw to loop body.
      // This instructions are not associated with a variable and are not used
      // outside the loop.
      if (auto *Phi{dyn_cast<PHINode>(&I)}) {
        SmallVector<DbgVariableIntrinsic *, 4> DbgInsts;
        findDbgUsers(DbgInsts, Phi);
        if (DbgInsts.empty() && !any_of_user_insts(*Phi, [L](auto *U) {
              return isa<Instruction>(U) && !L->contains(cast<Instruction>(U));
            }))
          continue;
      }
      // Collect users of a specified instruction which are located outside the
      // loop.
      for_each_user_insts(I, [this, &I, L, &OutwardDefs](Value *V) {
        if (auto *UI = dyn_cast<Instruction>(V)) {
          if (L->contains(UI))
            return;
          SmallVector<DIMemoryLocation, 4> DILocs;
          findMetadata(&I, makeArrayRef(UI), *mDT, DILocs);
          for (auto &DILoc : DILocs)
            OutwardDefs.try_emplace(DILoc).first->second.push_back(UI);
        }
      });
      // If instruction computes a value of a variable, remember it.
      // May be it is better to call this function for each instruction in
      // ExitInsts separately because processing of all instruction is more
      // conservative. In some cases a variable may have a value after one
      // exit and it may not have a value after another exit. This variable
      // could be a dynamic private (not private), so we simplify the check.
      SmallVector<DbgValueInst *, 4> DbgInsts;
      findDbgValues(DbgInsts, &I);
      bool HasDbgInLoop{false};
      for (auto *DVI : DbgInsts)
        if (HasDbgInLoop |= L->contains(DVI))
          Privates.insert(DIMemoryLocation::get(DVI));
      // Ignore values which come outside of the loop or from a previous loop
      // iteration but are not associated with any variable. This phi-nodes
      // just transits a value which maybe even isn't used inside the loop.
      // However, we should collect uses of this instruction outside the loop,
      // so do not move this check above.
      if (!HasDbgInLoop && isa<PHINode>(I) && I.getParent() == Head)
        continue;
      SmallPtrSet<Instruction *, 4> VisitedInsts{ &I };
      // If boolean value is set to true then value of a variable has been
      // computed on previous iteration and data dependence exists.
      SmallVector<PointerIntPair<Instruction *, 1, bool>, 4> Worklist;
      Worklist.emplace_back(&I, isa<PHINode>(I) && I.getParent() == Head);
      do {
        auto CurrInst = Worklist.pop_back_val();
        for (auto &Op : CurrInst.getPointer()->operands()) {
          SmallVector<DIMemoryLocation, 4> DILocs;
          if (auto *Phi = dyn_cast<PHINode>(CurrInst.getPointer()))
            findMetadata(Op, makeArrayRef(&Phi->getIncomingBlock(Op)->back()),
                         *mDT, DILocs);
          else
            findMetadata(Op, makeArrayRef(CurrInst.getPointer()), *mDT, DILocs);
          if (!DILocs.empty()) {
            if (CurrInst.getInt()) {
              OutwardUses.insert(DILocs.begin(), DILocs.end());
            } else if (isa<Instruction>(Op) &&
                       L->contains(cast<Instruction>(Op))) {
              Privates.insert(DILocs.begin(), DILocs.end());
            } else {
              SmallVector<DIMemoryLocation, 4> DILocsInHead;
              findMetadata(Op, makeArrayRef(&Head->front()), *mDT,
                           DILocsInHead);
              // In some cases Op may be executed before the loop or it could be
              // a constant expression. However, if memory location has other
              // value before the loop it means that assignment statement is
              // placed inside the loop. This assignment statement may use
              // other variable which was calculated by Op to set value of
              // analyzed memory location. In this case it can be privitized.
              for (auto &DIDef : DILocs)
                if (!llvm::count(DILocsInHead, DIDef))
                  Privates.insert(DIDef);
                else
                  OutwardUses.insert(DIDef);
            }
          } else {
            if (auto *OpI = dyn_cast<Instruction>(&Op))
              if (VisitedInsts.insert(OpI).second)
                Worklist.emplace_back(OpI, CurrInst.getInt() ||
                  isa<PHINode>(Op) && OpI->getParent() == Head);
          }
        }
      } while (!Worklist.empty());
    }
  }
  SmallVector<BasicBlock *, 1> ExitBlocks;
  L->getExitBlocks(ExitBlocks);
  SmallVector<LiveSet *, 1> ExitLives;
  transform(ExitBlocks, std::back_inserter(ExitLives), [this](auto *BB) {
    auto DFB{mRegionInfo->getRegionFor(BB)};
    assert(DFB && "Data-flow region must not be null!");
    auto LiveItr{mLiveInfo->find(DFB)};
    assert(LiveItr != mLiveInfo->end() &&
           "Live memory description must not be null!");
    return LiveItr->template get<LiveSet>().get();
  });
  for (auto &Candidate : Privates) {
    if (OutwardUses.count(Candidate))
      continue;
    auto MD{findInAliasTreeMapping(L->getHeader()->getParent(), Candidate)};
    bool IsFromMapping{MD != nullptr};
    if (!MD)
      MD = getRawDIMemoryIfExists(L->getHeader()->getContext(), Candidate);
    if (!MD)
      continue;
    auto DIMTraitItr = Pool.find_as(MD);
    if (DIMTraitItr == Pool.end() || DIMTraitItr->getMemory()->isOriginal() ||
        !handleLoopEmptyBindings(L, *DIMTraitItr, Pool, DIAliasSTR) ||
        !DIMTraitItr->is_any<trait::Anti, trait::Flow, trait::Output,
                             trait::LastPrivate, trait::SecondToLastPrivate,
                             trait::FirstPrivate, trait::DynamicPrivate>() ||
        isLockedTrait(*DIMTraitItr, LockedTraits, DIAliasSTR))
      continue;
    LLVM_DEBUG(if (DWLang) {
      dbgs() << "[DA DI]: update traits for ";
      printDILocationSource(*DWLang, *DIMTraitItr->getMemory(), dbgs());
      dbgs() << "\n";
    });
    // Check if a partially promoted memory location is live after an exit from
    // the loop,
    auto isPartiallyPromotedLive = [this, IsFromMapping, &DIMTraitItr,
                                    &Candidate, &OutwardDefs, &AliasSTR]() {
      auto DIEM{dyn_cast<DIEstimateMemory>(DIMTraitItr->getMemory())};
      if (!IsFromMapping || !DIEM || DIEM->emptyBinding() ||
          DIEM->getExpression()->getNumOperands() != 0)
        return true;
      auto Size{DIEM->getSize()};
      auto OutwardDefsItr{OutwardDefs.find(Candidate)};
      if (OutwardDefsItr == OutwardDefs.end())
        return false;
      SmallVector<BasicBlock *, 1> BlocksToCheck;
      SmallVector<LiveSet *, 1> LivesToCheck;
      for (auto *UI : OutwardDefsItr->second) {
        if (auto *SI{dyn_cast<StoreInst>(UI)};
            SI && any_of(*DIEM, [SI](auto &VH) {
              return VH && SI->getPointerOperand() == VH;
            })) {
          BlocksToCheck.push_back(UI->getParent());
          auto DFB{mRegionInfo->getRegionFor(UI->getParent())};
          assert(DFB && "Data-flow region must not be null!");
          auto LiveItr{mLiveInfo->find(DFB)};
          assert(LiveItr != mLiveInfo->end() &&
                 "Live memory description must not be null!");
          LivesToCheck.push_back(LiveItr->get<LiveSet>().get());
          continue;
        }
        return true;
      }
      SmallPtrSet<AliasNode *, 4> Nodes;
      auto IsLive{false};
      for (auto &VH : *DIEM) {
        if (!VH)
          continue;
        Nodes.insert(mAT->find(MemoryLocation(VH, Size))->getAliasNode(*mAT));
        IsLive |= any_of(LivesToCheck, [&VH, &Size](auto *LS) {
          return LS->getOut().overlap(MemoryLocation{VH, Size});
        });
      }
      IsLive |=
          any_of(BlocksToCheck, [this, &Nodes, &AliasSTR](BasicBlock *BB) {
            bool IsLive{false};
            for_each_memory(
                *BB, *mTLI,
                [this, &Nodes, &AliasSTR,
                 &IsLive](Instruction &I, MemoryLocation &&Loc, unsigned,
                          AccessInfo R, AccessInfo) {
                  if (IsLive || R == AccessInfo::No)
                    return;
                  auto *EM{mAT->find(Loc)};
                  IsLive |= any_of(Nodes, [this, EM, &AliasSTR](auto *AN) {
                    return !AliasSTR.isUnreachable(EM->getAliasNode(*mAT), AN);
                  });
                },
                [this, &Nodes, &AliasSTR, &IsLive](Instruction &I, AccessInfo R,
                                                   AccessInfo) {
                  if (IsLive || R == AccessInfo::No)
                    return;
                  auto *UN{mAT->findUnknown(I)};
                  IsLive |= any_of(Nodes, [this, UN, &AliasSTR](auto *AN) {
                    return !AliasSTR.isUnreachable(UN, AN);
                  });
                });
            return IsLive;
          });
      return IsLive;
    };
    // Look up for users of an analyzed variable outside the loop.
    if (OutwardDefs.count(Candidate) &&
        DIMTraitItr->is<trait::UseAfterLoop>() && isPartiallyPromotedLive()) {
      if (DIMTraitItr->is_any<trait::Anti, trait::Flow, trait::Output>()) {
        // TODO (kaniandr@gmail.com): do not mark variables as a first private
        // if there is at least one iteration in the loop.
        DIMTraitItr->set<trait::FirstPrivate, trait::DynamicPrivate>();
        LLVM_DEBUG(dbgs() << "[DA DI]: first private variable found\n");
        LLVM_DEBUG(dbgs() << "[DA DI]: dynamic private variable found\n");
        ++NumTraits.get<trait::FirstPrivate>();
        ++NumTraits.get<trait::DynamicPrivate>();
      }
    } else {
      DIMTraitItr->set<trait::Private>();
      DIMTraitItr->unset<trait::UseAfterLoop>();
      LLVM_DEBUG(dbgs() << "[DA DI]: private variable found\n");
      ++NumTraits.get<trait::Private>();
      --NumTraits.get<trait::UseAfterLoop>();
    }
  }
}

void DIDependencyAnalysisPass::analyzePromoted(Loop *L,
    Optional<unsigned> DWLang,
    const SpanningTreeRelation<AliasTree *> &AliasSTR,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const DIMemory *> LockedTraits, DIMemoryTraitRegionPool &Pool) {
  assert(L && "Loop must not be null!");
  LLVM_DEBUG(dbgs() << "[DA DI]: process loop at ";
             L->getStartLoc().print(dbgs()); dbgs() << "\n");
  for (auto &DIMTrait : Pool) {
    // Do not use hear `handleLoopEmptyBindings` because variable can be
    // promoted for a loop but access to its pointer without dereference is
    // steal allowed.
    // Otherwise we loose 'address access' traits for the following example:
    // int X;
    // long long bar() { return (long long)&X; }
    // long long foo() {
    //   long long S = 0;
    //   for (int I = 0; I < 10; ++I)
    //     S += bar();
    //   return S;
    // }
    if (DIMTrait.getMemory()->isOriginal() ||
        !DIMTrait.getMemory()->emptyBinding() ||
        isLockedTrait(DIMTrait, LockedTraits, DIAliasSTR))
      continue;
    DIMTrait.unset<trait::AddressAccess>();
  }
  analyzePrivatePromoted(L, DWLang, AliasSTR, DIAliasSTR, LockedTraits, Pool);
  // If there is no preheader induction and reduction analysis will fail.
  if (!L->getLoopPreheader())
    return;
  BasicBlock *Header = L->getHeader();
  Function &F = *Header->getParent();
  // Enable analysis of reductions in case of real variables.
  FastMathFlags FMF;
  FMF.setNoNaNs(
      F.getFnAttribute("no-nans-fp-math").getValueAsBool());
  FMF.setNoSignedZeros(
      F.getFnAttribute("no-signed-zeros-fp-math").getValueAsBool());
  if (!FMF.noNaNs())
    F.addFnAttr("no-nans-fp-math", "true");
  if (!FMF.noSignedZeros())
    F.addFnAttr("no-signed-zeros-fp-math", "true");
  for (auto I = L->getHeader()->begin(); isa<PHINode>(I); ++I) {
    auto *Phi = cast<PHINode>(I);
    RecurrenceDescriptor RD;
    InductionDescriptor ID;
    PredicatedScalarEvolution PSE(*mSE, *L);
    if (RecurrenceDescriptor::isReductionPHI(Phi, L, RD)) {
      auto RK = getReductionKind(RD);
      updateTraits(L, Phi, *mDT, DWLang, LockedTraits, DIAliasSTR, Pool, {},
                   [RK](DIMemoryTrait &T) {
        T.set<trait::Reduction>(new trait::DIReduction(RK));
        LLVM_DEBUG(dbgs() << "[DA DI]: reduction found\n");
        ++NumTraits.get<trait::Reduction>();
      });
    } else if (InductionDescriptor::isInductionPHI(Phi, L, PSE, ID)) {
      trait::DIInduction::Constant Start, Step, BackedgeCount;
      if (mSE->isSCEVable(ID.getStartValue()->getType()))
        if (auto *C = dyn_cast<SCEVConstant>(mSE->getSCEV(ID.getStartValue())))
          Start = APSInt(C->getAPInt());
      if (auto *C = dyn_cast<SCEVConstant>(ID.getStep()))
        Step = APSInt(C->getAPInt());
      if (Start && Step && mSE->hasLoopInvariantBackedgeTakenCount(L))
        if (auto *C = dyn_cast<SCEVConstant>(mSE->getBackedgeTakenCount(L)))
          BackedgeCount = APSInt(C->getAPInt());
      // We set variable as induction if it has not been marked as privitizable
      // however it obtains its value in the beginning of loop iteration.
      auto MDSerchIterationUsers =
          L->getHeader()->getFirstNonPHIOrDbgOrLifetime();
      updateTraits(
          L, Phi, *mDT, DWLang, LockedTraits, DIAliasSTR, Pool,
          makeArrayRef(MDSerchIterationUsers),
          [&ID, &Start, &Step, &BackedgeCount](DIMemoryTrait &T) {
        auto DIEM = dyn_cast<DIEstimateMemory>(T.getMemory());
        if (!DIEM)
          return;
        SourceUnparserImp Unparser(
            DIMemoryLocation::get(
                const_cast<DIVariable *>(DIEM->getVariable()),
                const_cast<DIExpression *>(DIEM->getExpression()), nullptr,
                DIEM->isTemplate(), DIEM->isAfterPointer()),
            true /*order of dimensions is not important here*/);
        if (!Unparser.unparse() || Unparser.getIdentifiers().empty())
          return;
        LLVM_DEBUG(dbgs() << "[DA DI]: induction found\n");
        ++NumTraits.get<trait::Induction>();
        auto Id = Unparser.getIdentifiers().back();
        assert(Id && "Identifier must not be null!");
        auto DITy = isa<DIVariable>(Id) ?
          stripDIType(cast<DIVariable>(Id)->getType()) :
          dyn_cast<DIDerivedType>(Id);
        while (DITy && isa<DIDerivedType>(DITy))
          DITy = stripDIType(cast<DIDerivedType>(DITy)->getBaseType());
        if (auto DIBasicTy = dyn_cast_or_null<DIBasicType>(DITy)) {
          auto Encoding = DIBasicTy->getEncoding();
          bool IsSigned;
          if (IsSigned = (Encoding == dwarf::DW_ATE_signed) ||
              !(IsSigned = !(Encoding == dwarf::DW_ATE_unsigned))) {
            if (Start)
              Start->setIsSigned(IsSigned);
            if (Step)
              Step->setIsSigned(IsSigned);
            trait::DIInduction::Constant End;
            if (Start && Step && BackedgeCount) {
              BackedgeCount->setIsSigned(IsSigned);
              End = *Start + *BackedgeCount * *Step;
            }
            T.set<trait::Induction>(
              new trait::DIInduction(ID.getKind(), Start, End, Step));
            return;
          }
        }
        T.set<trait::Induction>(new trait::DIInduction(ID.getKind()));
      });
    } else {
      propagateReduction(Phi, L, DWLang, DIAliasSTR, LockedTraits, Pool);
    }
  }
  if (!FMF.noNaNs())
    F.addFnAttr("no-nans-fp-math", "false");
  if (!FMF.noSignedZeros())
    F.addFnAttr("no-signed-zeros-fp-math", "false");
}

void DIDependencyAnalysisPass::propagateReduction(PHINode *Phi,
    Loop *L, Optional<unsigned> DWLang,
    const tsar::SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const tsar::DIMemory *> LockedTraits,
    tsar::DIMemoryTraitRegionPool &Pool) {
  // Collect reduction candidates.
  SmallVector<DIMemoryLocation, 2> DILocs;
  for (const auto &Incoming : Phi->incoming_values()) {
    if (!L->contains(Phi->getIncomingBlock(Incoming)))
      continue;
    LLVM_DEBUG(dbgs() << "[DA DI]: check whether promoted location ";
    printLocationSource(dbgs(), Incoming, mDT); dbgs() << " is reduction\n");
    Instruction * Users[] = { &Phi->getIncomingBlock(Incoming)->back() };
    findMetadata(Incoming, Users, *mDT, DILocs);
  }
  if (DILocs.empty())
    return;
  // This check whether there is a copy (X=Y) from some of reduction candidates
  // to a variable which is not presented in a list of reduction candidates.
  auto hasCopyFromReduction = [&DILocs](Instruction *I) {
    SmallVector<DbgValueInst *, 2> DbgValues;
    findDbgValues(DbgValues, I);
    return any_of(DbgValues, [&DILocs](DbgValueInst *DVI) {
      return hasDeref(*DVI->getExpression()) ||
        !is_contained(DILocs, DIMemoryLocation::get(DVI));
    });
  };
  if (hasCopyFromReduction(Phi))
    return;
  SmallVector<DIMemoryTraitRegionPool::iterator, 2> Traits;
  auto allowToUpdate =
    [&Pool, &LockedTraits, &DIAliasSTR, &Traits, &L](DIMemoryLocation &DILoc) {
    auto *MD = getRawDIMemoryIfExists(DILoc.Var->getContext(), DILoc);
    if (!MD)
      return false;
    auto DIMTraitItr = Pool.find_as(MD);
    if (DIMTraitItr == Pool.end() ||
      DIMTraitItr->getMemory()->isOriginal() ||
      !handleLoopEmptyBindings(L, *DIMTraitItr, Pool, DIAliasSTR) ||
      !DIMTraitItr->is<trait::Anti, trait::Flow, trait::Output>() ||
      isLockedTrait(*DIMTraitItr, LockedTraits, DIAliasSTR))
      return false;
    Traits.push_back(DIMTraitItr);
    return true;
  };
  if (!all_of(DILocs, allowToUpdate))
    return;
  // This check whether result of this instruction is a value of some
  // of reduction candidates (To) or this instruction is associated
  // with copy from (From) reduction candidates
  // to a variable which is not presented in a list of reduction candidates.
  struct ReductionCopy { bool To, From; };
  auto useReduction = [&DILocs](Instruction *I) -> ReductionCopy {
    SmallVector<DbgValueInst *, 2> DbgValues;
    findDbgValues(DbgValues, I);
    ReductionCopy Copy{ false, false };
    any_of(DbgValues, [&DILocs, &Copy](DbgValueInst *DVI) {
      bool Tmp = !hasDeref(*DVI->getExpression()) &&
        is_contained(DILocs, DIMemoryLocation::get(DVI));
      Copy.To |= Tmp;
      Copy.From |= !Tmp;
      return Copy.To && Copy.From;
    });
    return Copy;
  };
  SmallPtrSet<Loop *, 1> ReductionLoops;
  SmallPtrSet<Instruction *, 8> LCSSAPhis{ Phi };
  Optional<trait::Reduction::Kind> RedKind;
  // Check whether inner loop does not allow some of candidates be a
  // reduction variable in outer loop.
  //
  // If candidate is a reduction in the inner loop update reduction kind,
  // remember loop and LCSSA Phi node which is associated with reduction.
  auto isLoopPreventReduction = [this, &useReduction, &hasCopyFromReduction,
                                 &Traits, &ReductionLoops, &LCSSAPhis,
                                 &RedKind](Loop *InnerL) {
    if (!InnerL->getLoopID())
      return false;
    auto InnerPoolItr = mTraitPool->find(InnerL->getLoopID());
    // Pool of traits is empty if the inner loop is located in a function
    // which is called in the outer loop. After internal inlining these
    // loop are in the same function.
    if (InnerPoolItr == mTraitPool->end())
      return false;
    auto &InnerPool = *InnerPoolItr->get<tsar::Pool>();
    for (auto &DIMTraitItr : Traits) {
      auto InnerTraitItr = InnerPool.find_as(DIMTraitItr->getMemory());
      if (InnerTraitItr == InnerPool.end())
        return false;
      if (!InnerTraitItr->is<trait::Reduction>())
        return true;
      auto *Red = InnerTraitItr->get<trait::Reduction>();
      if (!Red || !*Red)
        RedKind = trait::DIReduction::RK_NoReduction;
      else if (!RedKind)
        RedKind = Red->getKind();
      else if (*RedKind != Red->getKind())
        RedKind = trait::DIReduction::RK_NoReduction;
      ReductionLoops.insert(InnerL);
      for (auto *BB : InnerL->blocks())
        for (const auto &I : *BB)
          for (auto &U : I.uses())
            if (auto *UI = dyn_cast<PHINode>(U.getUser())) {
              auto *UserBB = UI->getParent();
              if (InnerL->contains(UserBB))
                continue;
              SmallPtrSet<Instruction *, 4> Visited;
              do {
                ReductionCopy Copy;
                do {
                  Visited.insert(UI);
                  Copy = useReduction(UI);
                  // Skip intermediate phi-nodes which only forward value.
                  if (!Copy.To && !Copy.From && UI->hasNUses(1) &&
                      UI->getNumIncomingValues() == 1 &&
                      !InnerL->contains(UserBB = UI->getParent()))
                    UI = dyn_cast<PHINode>(*UI->user_begin());
                  else
                    break;
                } while (UI && !Visited.count(UI));
                if (!UI || !Copy.To)
                  continue;
                if (Copy.From)
                  return true;
                LCSSAPhis.insert(UI);
                // We also want to remember self-copying instructions.
                if (UI->hasNUses(1) && UI->getNumIncomingValues() == 1 &&
                    !InnerL->contains(UserBB = UI->getParent()))
                  UI = dyn_cast<PHINode>(*UI->user_begin());
                else
                  break;
              } while (UI && !Visited.count(UI));
            }
    }
    return false;
  };
  if (any_of(*L, isLoopPreventReduction) || !RedKind)
    return;
  // Check whether some of reduction candidates are accessed
  // in a loop except  inner loops in which these candidates
  // are reductions.
  auto isUsedInLoop =
    [this, &DILocs, &ReductionLoops, &LCSSAPhis, Phi, L](Instruction *I) {
    for (const auto &U : I->uses()) {
      if (!isa<Instruction>(U.getUser()))
        return true;
      auto *UI = cast<Instruction>(U.getUser());
      if (!L->contains(UI->getParent()))
        continue;
      auto *UseL = mLI->getLoopFor(UI->getParent());
      if (!LCSSAPhis.count(UI) && (UseL == L || !ReductionLoops.count(UseL)))
        return true;
    }
    return false;
  };
  if (any_of(LCSSAPhis, isUsedInLoop))
    return;
  for (auto &DIMTraitItr : Traits) {
    LLVM_DEBUG(if (DWLang) {
      dbgs() << "[DA DI]: update traits for ";
      printDILocationSource(*DWLang, *DIMTraitItr->getMemory(), dbgs());
      dbgs() << "\n";
    });
    if (*RedKind != trait::DIReduction::RK_NoReduction)
      DIMTraitItr->set<trait::Reduction>(new trait::DIReduction(*RedKind));
    else
      DIMTraitItr->set<trait::Reduction>();
    LLVM_DEBUG(dbgs() << "[DA DI]: reduction found\n");
    ++NumTraits.get<trait::Reduction>();
  }
}

void DIDependencyAnalysisPass::analyzeNode(DIAliasMemoryNode &DIN,
    Optional<unsigned> DWLang,
    const SpanningTreeRelation<AliasTree *> &AliasSTR,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const DIMemory *> LockedTraits, const GlobalOptions &GlobalOpts,
    const Loop &L, DependenceSet &DepSet, DIDependenceSet &DIDepSet,
    DIMemoryTraitRegionPool &Pool) {
  assert(!DIN.empty() && "Alias node must contain memory locations!");
  auto *AN = findBoundAliasNode(*mAT, AliasSTR, DIN);
  auto ATraitItr = AN ? DepSet.find_as(AN) : DepSet.end();
  DIDependenceSet::iterator DIATraitItr = DIDepSet.end();
  SmallPtrSet<const Value *, 16> MustNoAccessValues;
  for (auto &M : DIN) {
    LLVM_DEBUG(if (DWLang) {
      dbgs() << "[DA DI]: extract traits for ";
      printDILocationSource(*DWLang, M, dbgs());
      dbgs() << "\n";
    });
    auto DIMTraitItr = Pool.find_as(&M);
    auto *F{L.getHeader()->getParent()};
    bool NoRedundantMapping{false};
    if (auto MD{F->getMetadata("alias.tree.mapping")}) {
      auto MappingItr{
          find_if(MD->operands(), [ToFind = M.getAsMDNode()](auto &Op) {
            auto *OpMD{dyn_cast<MDNode>(Op)};
            if (!OpMD)
              return false;
            assert(OpMD->getNumOperands() == 2 &&
                   "Alias tree mapping node must contain two operands!");
            auto MappingMD{dyn_cast<MDNode>(OpMD->getOperand(0))};
            if (MappingMD == ToFind)
              return true;
            return false;
          })};
      if (MappingItr != MD->operands().end()) {
        if (auto *MDV{MetadataAsValue::getIfExists(
                F->getContext(), cast<MDNode>(*MappingItr)->getOperand(1))};
            MDV && any_of_user_insts(*MDV, [&L](auto *U) {
              return isa<DbgValueInst>(U) && L.contains(cast<DbgValueInst>(U));
            }))
          NoRedundantMapping = true;
      }
    }
    if (M.isOriginal() || M.emptyBinding() || ATraitItr == DepSet.end()) {
      if (DIMTraitItr == Pool.end())
        continue;
      if (!isLockedTrait(*DIMTraitItr, LockedTraits, DIAliasSTR)) {
        if (M.isOriginal() && ATraitItr != DepSet.end()) {
          LLVM_DEBUG(dbgs() << "[DA DI]: find traits for corrupted location\n");
          DIMemoryTrait DIMTrait(&M);
          DIMTrait.set<trait::NoAccess>();
          bool IsChanged = false;
          for (auto &Bind : M)
            if (combineWith(Bind, M, *mAT, ATraitItr, DIMTrait))
              IsChanged = true;
            else if (!isa<DIUnknownMemory>(M) ||
                     !cast<DIUnknownMemory>(M).isExec())
              MustNoAccessValues.insert(Bind);
          if (IsChanged) {
            clarify(DIMTrait, *DIMTraitItr);
          } else if (!NoRedundantMapping) {
            LLVM_DEBUG(dbgs() << "[DA DI]: use existing traits\n");
            LLVM_DEBUG(dbgs() << "[DA DI]: mark as redundant\n");
            DIMTraitItr->set<trait::Redundant>();
            DIMTraitItr->unset<trait::NoRedundant>();
          }
        } else if (!NoRedundantMapping) {
          LLVM_DEBUG(dbgs() << "[DA DI]: use existing traits\n");
          if ((!M.emptyBinding() && ATraitItr == DepSet.end()) ||
              (M.emptyBinding() && M.isOriginal())) {
            LLVM_DEBUG(dbgs() << "[DA DI]: mark as redundant\n");
            DIMTraitItr->set<trait::Redundant>();
            DIMTraitItr->unset<trait::NoRedundant>();
          }
        }
      } else {
          LLVM_DEBUG(dbgs() << "[DA DI]: use existing locked traits\n");
      }
      if (DIATraitItr == DIDepSet.end())
        DIATraitItr = DIDepSet.insert(DIAliasTrait(&DIN)).first;
      DIATraitItr->insert(DIMTraitItr);
      continue;
    }
    if (DIMTraitItr == Pool.end() ||
        !isLockedTrait(*DIMTraitItr, LockedTraits, DIAliasSTR)) {
      auto BindItr = M.begin();
      DIMemoryTrait DIMTrait(&M);
      DIMTrait.set<trait::NoAccess>();
      bool IsChanged = combineWith(*BindItr, M, *mAT, ATraitItr, DIMTrait);
      if (!IsChanged &&
          (!isa<DIUnknownMemory>(M) || !cast<DIUnknownMemory>(M).isExec()))
        MustNoAccessValues.insert(*BindItr);
      // Values binded to merged locations may be related to different estimate
      // memory locations with different traits. So, it is necessary to check
      // all binded values.
      if (M.isMerged()) {
        LLVM_DEBUG(dbgs() << "[DA DI]: find traits for merged location\n");
        auto BindItrE = M.end();
        for (++BindItr; BindItr != BindItrE; ++BindItr)
          if (combineWith(*BindItr, M, *mAT, ATraitItr, DIMTrait))
            IsChanged = true;
          else if (!isa<DIUnknownMemory>(M) ||
                   !cast<DIUnknownMemory>(M).isExec())
            MustNoAccessValues.insert(*BindItr);
      }
      if (IsChanged) {
        if (DIMTraitItr == Pool.end()) {
          LLVM_DEBUG(dbgs() << "[DA DI]: add new trait to pool\n");
          DIMTraitItr =
            Pool.try_emplace({ &M, &Pool }, std::move(DIMTrait)).first;
        } else {
          clarify(DIMTrait, *DIMTraitItr);
        }
      } else if (!NoRedundantMapping) {
        if (DIMTraitItr == Pool.end())
          continue;
        LLVM_DEBUG(dbgs() << "[DA DI]: use existing traits\n");
        LLVM_DEBUG(dbgs() << "[DA DI]: mark as redundant\n");
        DIMTraitItr->set<trait::Redundant>();
        DIMTraitItr->unset<trait::NoRedundant>();
      }
    }
    assert(DIMTraitItr != Pool.end() && "Trait must not be null!");
    if (DIATraitItr == DIDepSet.end())
      DIATraitItr = DIDepSet.insert(DIAliasTrait(&DIN)).first;
    DIATraitItr->insert(DIMTraitItr);
  }
  // We will collapse traits for promoted location only if this location is
  // used in a region. If location is explicitly used traits for locations
  // covered by it will be ignored, because the parent trait has been already
  // analyzed.
  struct {
    DIMemory *Memory = nullptr;
    bool Collapse = true;
    BitMemoryTrait Trait;
  } CurrentPromoted;
  if (DIATraitItr != DIDepSet.end() && isa<DIAliasEstimateNode>(DIN) &&
      DIN.size() == 1 && DIN.begin()->emptyBinding()) {
    auto DIMTraitItr = DIATraitItr->find(&*DIN.begin());
    if (DIMTraitItr != DIATraitItr->end()) {
      CurrentPromoted.Memory = &*DIN.begin();
      LLVM_DEBUG(if (DWLang) {
        dbgs() << "[DA DI]: node contains a single promoted location ";
        printDILocationSource(*DWLang, *CurrentPromoted.Memory, dbgs());
        dbgs() << "\n";
      });
      CurrentPromoted.Collapse = !(**DIMTraitItr).is<trait::ExplicitAccess>();
    }
  }
  // Collect locations from descendant nodes which are explicitly accessed.
  for (auto &Child : make_range(DIN.child_begin(), DIN.child_end())) {
    auto ChildTraitItr = DIDepSet.find_as(&Child);
    // If memory from this child is not accessed in a region then go to the next
    // child.
    if (ChildTraitItr == DIDepSet.end())
      continue;
    SmallVector<DIMemoryTraitRef, 8> DescendantTraits;
    for (auto &DIMTraitItr : *ChildTraitItr) {
      auto *DIM = DIMTraitItr->getMemory();
      // Alias trait contains not only locations from a corresponding alias
      // node. It may also contain locations from descendant nodes.
      auto N = DIM->getAliasNode();
      if (isa<DIAliasEstimateNode>(DIN) && isa<DIAliasEstimateNode>(N)) {
        if (CurrentPromoted.Memory) {
          LLVM_DEBUG(if (DWLang) {
            dbgs() << "[DA DI]: "
                   << (CurrentPromoted.Collapse ? "propagate" : "ignore")
                   << " traits of promoted location child ";
            printDILocationSource(*DWLang, *DIM, dbgs());
            dbgs() << "\n";
          });
          if (CurrentPromoted.Collapse)
            CurrentPromoted.Trait &= *DIMTraitItr;
          continue;
        } else if (!DIM->emptyBinding()) {
          // Locations from descendant estimate nodes have been already processed
          // implicitly when IR-level analysis has been performed and traits for
          // estimate memory locations have been fetched for nodes in IR-level
          // alias tree.
          continue;
        }
      }
      LLVM_DEBUG(if (DWLang) {
        dbgs() << "[DA DI]: extract traits from descendant node ";
        printDILocationSource(*DWLang, *DIM, dbgs());
        dbgs() << "\n";
      });
      DescendantTraits.push_back(DIMTraitItr);
    }
    if (!DescendantTraits.empty()) {
      if (DIATraitItr == DIDepSet.end())
        DIATraitItr = DIDepSet.insert(DIAliasTrait(&DIN)).first;
      for (auto &DIMTraitItr : DescendantTraits)
        DIATraitItr->insert(std::move(DIMTraitItr));
    }
  }
  if (CurrentPromoted.Memory && CurrentPromoted.Collapse &&
      CurrentPromoted.Trait != BitMemoryTrait::NoAccess) {
    DIMemoryTraitRef DIMTraitItr;
    std::tie(DIMTraitItr, std::ignore) =
      addOrUpdateInPool(*CurrentPromoted.Memory,
        CurrentPromoted.Trait.toDescriptor(1, NumTraits),
        LockedTraits, DIAliasSTR, Pool);
    DIMTraitItr->unset<trait::ExplicitAccess>();
    assert(DIATraitItr != DIDepSet.end() && "Trait must be already exist!");
    DIATraitItr->insert(DIMTraitItr);
  } else if (DIATraitItr == DIDepSet.end()) {
    return;
  }
  LLVM_DEBUG(dbgs() << "[DA DI]: search for redundant memory\n");
  RedundantSearch(*mAT, DWLang, *DIATraitItr, LockedTraits, DIAliasSTR,
    MustNoAccessValues).exec();
  LLVM_DEBUG(dbgs() << "[DA DI]: sanitize indirect accesses\n");
  IndirectAccessSanitizer(*mAT, DWLang, *DIATraitItr,
    GlobalOpts.IgnoreRedundantMemory).exec();
  combineTraits(GlobalOpts.IgnoreRedundantMemory, *DIATraitItr);
  // We do not update traits for each memory location (as in private recognition
  // pass) because these traits should be updated early (during analysis of
  // promoted locations or by the private recognition pass).
}

/// Recurse through all subloops and all loops  into LQ.
static void addLoopIntoQueue(DFNode *DFN, std::deque<DFLoop *> &LQ) {
  if (auto *DFL = dyn_cast<DFLoop>(DFN)) {
    LQ.push_front(DFL);
    for (auto *I : DFL->getRegions())
      addLoopIntoQueue(I, LQ);
  }
}

/// Check nodes with pointer dereference and investigate combined traits to
/// ignore some of traits for memory locations.
///
/// The following case is checked:
/// Alias Nodes (1): {x, *p}, (2) { ... p ... }.
/// If traits for (p) is 'private' then traits for *p could be ignored and
/// traits for (1) could be set to separate traits of x.
static DIMemoryTraitRef *mayIgnoreDereference(DIAliasTrait &Trait,
    const DIAliasTree &Tree, DIDependenceSet &DS,
    const DenseMap<DIVariable *, DIMemory *> &VarToMemory) {
  DIMemoryTraitRef *MainMemory = nullptr;
  for (auto &T : Trait) {
    if (isa<DIUnknownMemory>(T->getMemory()))
      return nullptr;
    auto *DIEM = cast<DIEstimateMemory>(T->getMemory());
    if (DIEM->hasDeref()) {
      auto MemoryItr = VarToMemory.find(DIEM->getVariable());
      if (MemoryItr != VarToMemory.end()) {
        auto TraitItr = DS.find_as(MemoryItr->second->getAliasNode());
        if (TraitItr != DS.end() && TraitItr->is<trait::Private>())
          continue;
      }
      return nullptr;
    }
    if (MainMemory)
      return nullptr;
    MainMemory = &T;
  }
  return MainMemory;
}

bool DIDependencyAnalysisPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  if (!GlobalOpts.AnalyzeLibFunc && hasFnAttr(F, AttrKind::LibFunc))
    return false;
  mDT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  mAT = &getAnalysis<EstimateMemoryPass>().getAliasTree();
  mLI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  mLiveInfo = &getAnalysis<LiveMemoryPass>().getLiveInfo();
  mRegionInfo = &getAnalysis<DFRegionInfoPass>().getRegionInfo();
  mTraitPool = &getAnalysis<DIMemoryTraitPoolWrapper>().get();
  mTLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F);
  auto &PI = getAnalysis<PrivateRecognitionPass>().getPrivateInfo();
  auto &DIAT = getAnalysis<DIEstimateMemoryPass>().getAliasTree();
  auto &DL = F.getParent()->getDataLayout();
  auto DWLang = getLanguage(F);
  SpanningTreeRelation<AliasTree *> AliasSTR(mAT);
  SpanningTreeRelation<const DIAliasTree *> DIAliasSTR(&DIAT);
  auto *DFF = cast<DFFunction>(mRegionInfo->getTopLevelRegion());
  std::deque<DFLoop *> LQ;
  for (auto *DFN : DFF->getRegions())
    addLoopIntoQueue(DFN, LQ);
  for (auto *DFL : LQ) {
    auto L = DFL->getLoop();
    /// TODO (kaniandr@gmail.com): use other identifier because LLVM identifier
    /// may be lost.
    if (!L->getLoopID())
      continue;
    assert(L->getLoopID() && "Identifier of a loop must be specified!");
    auto DILoop = L->getLoopID();
    LLVM_DEBUG(dbgs() << "[DA DI]: process "; TSAR_LLVM_DUMP(L->dump());
      if (DebugLoc DbgLoc = L->getStartLoc()) {
        dbgs() << "[DA DI]: loop at ";  DbgLoc.print(dbgs()); dbgs() << "\n";
      });
    DIMemoryTraitRegionPool *Pool{nullptr};
    if (mIsInitialization) {
      auto &PoolRef{(*mTraitPool)[DILoop]};
      LLVM_DEBUG(if (DWLang) allocatePoolLog(*DWLang, PoolRef));
      if (!PoolRef)
        PoolRef = std::make_unique<DIMemoryTraitRegionPool>();
      Pool = PoolRef.get();
    } else if (auto Itr{mTraitPool->find(DILoop)}; Itr != mTraitPool->end()) {
      LLVM_DEBUG(if (DWLang) allocatePoolLog(*DWLang, Itr->get<tsar::Pool>()));
      Pool = Itr->get<tsar::Pool>().get();
    } else {
      continue;
    }
    SmallVector<const DIMemory *, 4> LockedTraits;
    for (auto &T : *Pool)
      if (T.is<trait::Lock>())
        LockedTraits.push_back(T.getMemory());
    assert(PI.count(DFL) && "IR-level traits must be available for a loop!");
    auto &DepSet = PI.find(DFL)->get<DependenceSet>();
    auto &DIDepSet = mDeps.try_emplace(DILoop, DepSet.size()).first->second;
    analyzePromoted(L, DWLang, AliasSTR, DIAliasSTR, LockedTraits, *Pool);
    DenseMap<DIVariable *, DIMemory *> VarToMemory;
    for (auto *DIN : post_order(&DIAT)) {
      if (isa<DIAliasTopNode>(DIN))
        continue;
      analyzeNode(cast<DIAliasMemoryNode>(*DIN), DWLang, AliasSTR, DIAliasSTR,
        LockedTraits, GlobalOpts, *L, DepSet, DIDepSet, *Pool);
      for (auto &DIM : cast<DIAliasMemoryNode>(*DIN))
        if (auto *DIEM = dyn_cast<DIEstimateMemory>(&DIM))
          if (DIEM->getExpression()->getNumElements() == 0)
            VarToMemory.try_emplace(DIEM->getVariable(), DIEM);
    }
    LLVM_DEBUG(dbgs() << "[DA DI]: set traits for a top level node\n");
    auto TopDIN = DIAT.getTopLevelNode();
    auto TopTraitItr = DIDepSet.insert(DIAliasTrait(TopDIN)).first;
    for (auto &Child : make_range(TopDIN->child_begin(), TopDIN->child_end())) {
      auto ChildTraitItr = DIDepSet.find_as(&Child);
      if (ChildTraitItr == DIDepSet.end())
        continue;
      for (auto &DIMTraitItr : *ChildTraitItr)
        TopTraitItr->insert(DIMTraitItr);
    }
    combineTraits(GlobalOpts.IgnoreRedundantMemory, *TopTraitItr);
    for (auto &AT : DIDepSet) {
      if (AT.size() == 1 ||
          !AT.is_any<trait::Anti, trait::Flow, trait::Output>())
        continue;
      if (auto *T = mayIgnoreDereference(AT, DIAT, DIDepSet, VarToMemory)) {
        BitMemoryTrait BitTrait(**T);
        BitTrait = dropUnitFlag(BitTrait);
        bcl::trait::set(BitTrait.toDescriptor(0, NumTraits), AT);
      }
    }
    std::vector<const DIAliasNode *> Coverage;
    explicitAccessCoverage(DIDepSet, DIAT, Coverage,
      GlobalOpts.IgnoreRedundantMemory);
    // All descendant nodes for nodes in `Coverage` access some part of
    // explicitly accessed memory. The conservativeness of analysis implies
    // that memory accesses from this nodes arise loop carried dependencies.
    for (auto *N : Coverage)
      for (auto &Child : make_range(N->child_begin(), N->child_end()))
        for (auto *Descendant : make_range(df_begin(&Child), df_end(&Child))) {
          auto I = DIDepSet.find_as(Descendant);
          if (I != DIDepSet.end() && !I->is<trait::NoAccess>())
            I->set<trait::Flow, trait::Anti, trait::Output>();
        }
  }
  return false;
}

namespace {
template<class IgnoreTraitList>
struct TraitPrinter {
  /// Sorted list of traits to (print their in algoristic order).
  using SortedVarListT = std::set<std::string, std::less<std::string>>;

  explicit TraitPrinter(raw_ostream &OS, const DIAliasTree &DIAT,
    StringRef Offset, unsigned DWLang) :
    mOS(OS), mDIAT(&DIAT), mOffset(Offset), mDWLang(DWLang) {}

  template<class Trait> void operator()(
      ArrayRef<const DIAliasTrait *> TraitVector) {
    if (bcl::IsTypeExist<Trait, IgnoreTraitList>::value || TraitVector.empty())
      return;
    mOS << mOffset << Trait::toString() << ":\n";
    /// Sort traits to print their in algoristic order.
    std::vector<SortedVarListT> VarLists;
    auto less = [&VarLists](decltype(VarLists)::size_type LHS,
      decltype(VarLists)::size_type RHS) {
      return VarLists[LHS] < VarLists[RHS];
    };
    std::set<decltype(VarLists)::size_type, decltype(less)> ANTraitList(less);
    for (auto *AT : TraitVector) {
      VarLists.emplace_back();
      for (auto &T : *AT) {
        if (!bcl::is_contained<Trait, trait::AddressAccess,
                               trait::Redundant>::value &&
            T->is<trait::NoAccess>())
          continue;
        std::string Str;
        raw_string_ostream TmpOS(Str);
        printDILocationSource(mDWLang, *T->getMemory(), TmpOS);
        traitToStr(T->get<Trait>(), TmpOS);
        VarLists.back().insert(TmpOS.str());
      }
      ANTraitList.insert(VarLists.size() - 1);
    }
    mOS << mOffset;
    auto ANTraitItr = ANTraitList.begin(), EI = ANTraitList.end();
    for (auto &T : VarLists[*ANTraitItr])
      mOS << " " << T;
    for (++ANTraitItr; ANTraitItr != EI; ++ANTraitItr) {
      mOS << " |";
      for (auto &T : VarLists[*ANTraitItr])
        mOS << " " << T;
    }
    mOS << "\n";
  }

  /// Prints description of a trait into a specified stream.
  void traitToStr(trait::DIDependence *Dep, raw_string_ostream &OS) {
    if (!Dep || Dep->getLevels() == 0)
      return;
    OS << ":[";
    unsigned I = 0, EI = Dep->getLevels();
    if (Dep->getDistance(I).first)
      OS << *Dep->getDistance(I).first;
    OS << ":";
    if (Dep->getDistance(I).second)
      OS << *Dep->getDistance(I).second;
    for (++I; I < EI; ++I) {
      OS << ",";
      if (Dep->getDistance(I).first)
        OS << *Dep->getDistance(I).first;
      OS << ":";
      if (Dep->getDistance(I).second)
        OS << *Dep->getDistance(I).second;
    }
    OS << "]";
  }

  /// Prints description of a trait into a specified stream.
  void traitToStr(trait::DIInduction *Induct, raw_string_ostream &OS) {
    if (!Induct)
      return;
    OS << ":[";
    switch (Induct->getKind()) {
      case trait::DIInduction::InductionKind::IK_IntInduction:
        OS << "Int"; break;
      case trait::DIInduction::InductionKind::IK_PtrInduction:
        OS << "Ptr"; break;
      case trait::DIInduction::InductionKind::IK_FpInduction:
        OS << "Fp"; break;
      default:
        llvm_unreachable("Unsupported kind of induction!");
        break;
    }
    OS << ",";
    if (Induct->getStart())
      OS << *Induct->getStart();
    OS << ",";
    if (Induct->getEnd())
      OS << *Induct->getEnd();
    OS << ",";
    if (Induct->getStep())
      OS << *Induct->getStep();
    OS << "]";
  }

  /// Prints description of a trait into a specified stream.
  void traitToStr(trait::DIReduction *Red, raw_string_ostream &OS) {
    if (!Red)
      return;
    switch (Red->getKind()) {
    case trait::DIReduction::RK_Add: OS << ":add"; break;
    case trait::DIReduction::RK_Mult: OS << ":mult"; break;
    case trait::DIReduction::RK_Or: OS << ":or"; break;
    case trait::DIReduction::RK_And: OS << ":and"; break;
    case trait::DIReduction::RK_Xor: OS << ":xor"; break;
    case trait::DIReduction::RK_Max: OS << ":max"; break;
    case trait::DIReduction::RK_Min: OS << ":min"; break;
    default: llvm_unreachable("Unsupported kind of reduction!"); break;
    }
  }

  /// Prints description of a trait into a specified stream.
  void traitToStr(void *Dep, raw_string_ostream &OS) {}

  llvm::raw_ostream &mOS;
  const DIAliasTree *mDIAT;
  std::string mOffset;
  unsigned mDWLang;
};
/// If trait is set and its tag is presented in TraitList then store
/// string representation of a trait in a list of traits.
template<class TraitList, class StorageTraitList = TraitList>
struct StoreIfInList {
  /// Sorted list of traits to (print their in algoristic order).
  using SortedVarListT = std::set<std::string, std::less<std::string>>;

  /// Map from tag to a sorted list of traits.
  using TraitMap =
    bcl::tags::get_tagged_tuple_t<SortedVarListT, StorageTraitList>;

  StoreIfInList(unsigned DWLang, const DIMemoryTrait &T, TraitMap &Map) :
    mDWLang(DWLang), mTrait(T), mTraits(Map) {}

  template<class Trait> void operator()() {
    insert<Trait>(bcl::IsTypeExist<Trait, TraitList>());
  }

  template<class Trait> void insert(std::true_type) {
    if (mTraitStr.empty()) {
      raw_string_ostream TmpOS(mTraitStr);
      printDILocationSource(mDWLang, *mTrait.getMemory(), TmpOS);
    }
    mTraits.template get<Trait>().insert(mTraitStr);
  }

  template<class Trait> void insert(std::false_type) {}

private:
  unsigned mDWLang;
  const DIMemoryTrait &mTrait;
  TraitMap &mTraits;
  std::string mTraitStr;
};

/// Functor to print some traits for memory locations which are stored in a
/// specified list.
template<class TraitList>
struct TraitListPrinter {
  using TraitMap = typename StoreIfInList<TraitList>::TraitMap;

  template<class TagT> void operator()() {
    if (Traits.template get<TagT>().empty())
      return;
    OS << Offset << TagT::toString() << " (separate):\n" << Offset;
    for (auto &T : Traits.template get<TagT>())
      OS << " " << T;
    OS << "\n";
  }

  llvm::raw_ostream &OS;
  std::string Offset;
  TraitMap &Traits;
};
}

void DIDependencyAnalysisPass::print(raw_ostream &OS, const Module *M) const {
  auto &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &DIAT = getAnalysis<DIEstimateMemoryPass>().getAliasTree();
  if (!GlobalOpts.AnalyzeLibFunc &&
      hasFnAttr(DIAT.getFunction(), AttrKind::LibFunc))
    return;
  auto DWLang = getLanguage(DIAT.getFunction());
  if (!DWLang) {
    DiagnosticInfoUnsupported Diag(DIAT.getFunction(),
     "unknown source language for function", findMetadata(&DIAT.getFunction()),
      DS_Warning);
    M->getContext().diagnose(Diag);
    return;
  }
  for_each_loop(LpInfo, [this, M, &GlobalOpts, &DIAT, &OS, &DWLang](Loop *L) {
    DebugLoc Loc = L->getStartLoc();
    if (Loc && Loc.getInlinedAt())
      return;
    std::string Offset(L->getLoopDepth(), ' ');
    OS << Offset;
    OS << "loop at depth " << L->getLoopDepth() << " ";
    tsar::print(OS, Loc, GlobalOpts.PrintFilenameOnly);
    OS << "\n";
    Offset.append("  ");
    auto DILoop = L->getLoopID();
    if (!DILoop) {
      DiagnosticInfoUnsupported Diag(DIAT.getFunction(),
        "loop has not been analyzed due to absence of debug information",
        Loc, DS_Warning);
      M->getContext().diagnose(Diag);
      return;
    }
    auto Info = mDeps.find(DILoop);
    assert(Info != mDeps.end() && "Results of analysis are not found!");
    using TraitMap = bcl::StaticTraitMap<
      std::vector<const DIAliasTrait *>, MemoryDescriptor>;
    TraitMap TM;
    DenseSet<const DIAliasNode *> Coverage, RedundantCoverage;
    accessCoverage<bcl::SimpleInserter>(Info->get<DIDependenceSet>(),
      DIAT, Coverage, GlobalOpts.IgnoreRedundantMemory);
    if (GlobalOpts.IgnoreRedundantMemory)
      accessCoverage<bcl::SimpleInserter>(Info->get<DIDependenceSet>(),
        DIAT, RedundantCoverage, false);
    // List of traits which should be printed if they have been set for
    // a memory location separately (it may not be set for the whole alias node).
    using SeparateTrateList = bcl::TypeList<
      trait::ExplicitAccess, trait::Redundant, trait::Lock, trait::AddressAccess,
      trait::NoPromotedScalar, trait::DirectAccess, trait::IndirectAccess>;
    StoreIfInList<SeparateTrateList>::TraitMap SeparateTraits;
    for (auto &TS : Info->get<DIDependenceSet>()) {
      if (Coverage.count(TS.getNode())) {
        TS.for_each(
          bcl::TraitMapConstructor<const DIAliasTrait, TraitMap>(TS, TM));
        for (auto &DIMTraitItr : TS)
          DIMTraitItr->for_each(
            StoreIfInList<SeparateTrateList>{
              *DWLang, *DIMTraitItr, SeparateTraits});

      } else if (GlobalOpts.IgnoreRedundantMemory &&
                 RedundantCoverage.count(TS.getNode())) {
        TM.value<trait::Redundant>().push_back(&TS);
        for (auto &DIMTraitItr : TS)
          DIMTraitItr->for_each(
            StoreIfInList<bcl::TypeList<trait::Redundant>, SeparateTrateList>{
              *DWLang, *DIMTraitItr, SeparateTraits});
      }
    }
    using IgnoreTraitList = bcl::TypeList<trait::NoRedundant, trait::NoAccess,
      trait::DirectAccess, trait::IndirectAccess, trait::UseAfterLoop>;
    TM.for_each(
      TraitPrinter<IgnoreTraitList>{OS, DIAT, Offset, *DWLang});
    SeparateTrateList::for_each_type(
      TraitListPrinter<SeparateTrateList>{ OS, Offset, SeparateTraits });
  });
}

void DIDependencyAnalysisPass::getAnalysisUsage(AnalysisUsage &AU)  const {
  AU.addRequired<LiveMemoryPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<DFRegionInfoPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<PrivateRecognitionPass>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createDIDependencyAnalysisPass(bool IsInitialization) {
  return new DIDependencyAnalysisPass(IsInitialization);
}

void DIMemoryTraitHandle::deleted() {
  assert(mPool && "Pool of traits must not be null!");
  auto I = mPool->find_as(getMemoryPtr());
  if (I != mPool->end()) {
    LLVM_DEBUG(
      dbgs() << "[DA DI]: delete from pool metadata-level memory location ";
      printDILocationSource(dwarf::DW_LANG_C, *getMemoryPtr(), dbgs());
      dbgs() << "\n");
    mPool->erase(I);
  }
}

void DIMemoryTraitHandle::allUsesReplacedWith(DIMemory *M) {
  assert(M != getMemoryPtr() &&
    "Old and new memory locations must not be equal!");
  assert(mPool && "Pool of traits must not be null!");
  DIMemoryTraitRegionPool::persistent_iterator OldItr =
    mPool->find_as(getMemoryPtr());
  LLVM_DEBUG(dbgs() << "[DA DI]: " << (M->isMerged() ? "merge" : "replace")
                    << " in pool metadata-level memory location ";
             printDILocationSource(dwarf::DW_LANG_C, *getMemoryPtr(), dbgs());
             dbgs() << " with ";
             printDILocationSource(dwarf::DW_LANG_C, *M, dbgs());
             dbgs() << "\n");
  assert((mPool->find_as(M) == mPool->end() ||
    mPool->find_as(M)->getMemory() != M || M->isMerged()) &&
    "New memory location is already presented in the memory trait pool!");
  if (M->isMerged()) {
    auto DIMTraitItr = mPool->find_as(M);
    if (DIMTraitItr != mPool->end()) {
      LLVM_DEBUG(dbgs() << "[DA DI]: target location exist in pool\n");
      auto HasSpuriousDep =
          hasSpuriousDep(*OldItr) || hasSpuriousDep(*DIMTraitItr);
      bcl::TraitDescriptor<trait::Flow, trait::Anti, trait::Output> DepDptr;
      if (!HasSpuriousDep) {
        bcl::trait::set(*DIMTraitItr, DepDptr);
        bcl::trait::set(*OldItr, DepDptr);
      }
      // Do not move combineIf() after update() of DIMTrait because initial
      // value of DIMTrait is necessary to accurately combine attached values.
      combineIf<trait::Flow>(*OldItr, *DIMTraitItr);
      combineIf<trait::Anti>(*OldItr, *DIMTraitItr);
      combineIf<trait::Output>(*OldItr, *DIMTraitItr);
      auto CombinedTrait = BitMemoryTrait(*DIMTraitItr);
      CombinedTrait &= *OldItr;
      // Do not use `operator=` because already attached values should not be
      // lost.
      bcl::trait::update(CombinedTrait.toDescriptor(1, NumTraits), *DIMTraitItr);
      // Do not set dependence if it have not been set in any descriptor.
      if (!HasSpuriousDep)
        bcl::trait::update(DepDptr, *DIMTraitItr);
      if (DIMTraitItr->is<trait::NoRedundant>())
        DIMTraitItr->unset<trait::Redundant>();
      mPool->erase(OldItr);
      return;
    }
  }
  auto TS(std::move(OldItr->getSecond()));
  auto Pool = mPool;
  // Do not use members of handle after the call of erase(), because
  // it destroys this.
  Pool->erase(OldItr);
  auto IsOk = Pool->try_emplace({ M, Pool }, std::move(TS)).second;
  assert(IsOk && "Unable to insert memory location in a pool!");
}

namespace {
class DIMemoryTraitPoolStorage :
  public ImmutablePass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  DIMemoryTraitPoolStorage() : ImmutablePass(ID) {
    initializeDIMemoryTraitPoolStoragePass(*PassRegistry::getPassRegistry());
  }

  void initializePass() override {
    getAnalysis<DIMemoryTraitPoolWrapper>().set(mPool);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<DIMemoryTraitPoolWrapper>();
  }

private:
  DIMemoryTraitPool mPool;
};
}

char DIMemoryTraitPoolStorage::ID = 0;
INITIALIZE_PASS_BEGIN(DIMemoryTraitPoolStorage, "di-mem-trait-is",
  "Memory Trait Immutable Storage (Metadata)", true, true)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_END(DIMemoryTraitPoolStorage, "di-mem-trait-is",
  "Memory Trait Immutable Storage (Metadata)", true, true)

template<> char DIMemoryTraitPoolWrapper::ID = 0;
INITIALIZE_PASS(DIMemoryTraitPoolWrapper, "di-mem-trait-iw",
  "Memory Trait Immutable Wrapper (Metadata)", true, true)

ImmutablePass * llvm::createDIMemoryTraitPoolStorage() {
  return new DIMemoryTraitPoolStorage();
}
