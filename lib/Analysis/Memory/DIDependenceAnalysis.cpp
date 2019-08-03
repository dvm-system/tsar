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
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "Attributes.h"
#include "BitMemoryTrait.h"
#include "tsar_dbg_output.h"
#include "DIEstimateMemory.h"
#include "EstimateMemory.h"
#include "GlobalOptions.h"
#include "tsar_query.h"
#include "SourceUnparser.h"
#include "SpanningTreeRelation.h"
#include "tsar/Analysis/Memory/PrivateAnalysis.h"
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "da-di"

MEMORY_TRAIT_STATISTIC(NumTraits)

char DIDependencyAnalysisPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(DIDependencyAnalysisPass, "da-di",
  "Dependency Analysis (Metadata)", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass);
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass);
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass);
  INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper);
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper);
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
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

/// Convert IR-level representation of a dependence of
/// a specified type `Tag` to metadata-level representation.
///
/// \pre `Tag` must be one of `trait::Flow`, `trait::Anti`, `trati::Output`.
template<class Tag> void convertIf(
    const EstimateMemoryTrait &IRTrait, DIMemoryTrait &DITrait) {
  static_assert(std::is_same<Tag, trait::Flow>::value ||
    std::is_same<Tag, trait::Anti>::value ||
    std::is_same<Tag, trait::Output>::value, "Unknown type of dependence!");
  if (auto IRDep = IRTrait.template get<Tag>()) {
    if (auto *DIDep = DITrait.template get<Tag>()) {
      if (DIDep->isKnownDistance())
        return;
    }
    LLVM_DEBUG(dbgs() << "[DA DI]: update " << Tag::toString()
                      << " dependence\n");
    auto Dist = IRDep->getDistance();
    trait::DIDependence::DistanceRange DIDistRange;
    if (auto ConstDist = dyn_cast_or_null<SCEVConstant>(Dist.first))
      DIDistRange.first = APSInt(ConstDist->getAPInt());
    if (auto ConstDist = dyn_cast_or_null<SCEVConstant>(Dist.second))
      DIDistRange.second = APSInt(ConstDist->getAPInt());
    auto F = IRDep->getFlags();
    if (IRDep->isKnownDistance() &&
      (!DIDistRange.first || !DIDistRange.second))
      F |= trait::Dependence::UnknownDistance;
    DITrait.template set<Tag>(new trait::DIDependence(F, DIDistRange));
  }
}

/// Store traits for a specified memory `M` in a specified pool. Add new
/// trait if `M` is not stored in pool.
///
/// If traits for `M` already exist in a poll and should not be updated
/// (for example, should be locked) the second returned value is `false`.
std::pair<DIMemoryTraitRef, bool> addOrUpdateInPool(DIMemory &M,
    const MemoryDescriptor &Dptr, ArrayRef<const DIMemory *> LockedTraits,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    DIMemoryTraitRegionPool &Pool) {
  auto DIMTraitItr = Pool.find_as(&M);
  LLVM_DEBUG(if (DIMTraitItr == Pool.end())
    dbgs() << "[DA DI]: add new trait to pool\n");
  if (DIMTraitItr == Pool.end())
    DIMTraitItr = Pool.try_emplace({ &M, &Pool }, Dptr).first;
  else if (!isLockedTrait(*DIMTraitItr, LockedTraits, DIAliasSTR))
    *DIMTraitItr = Dptr;
  else
    return std::make_pair(DIMTraitItr, false);
  return std::make_pair(DIMTraitItr, true);
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
    if (M.isOriginal() || M.emptyBinding())
      continue;
    findBoundAliasNodes(M, AT, BoundNodes);
    assert(!BoundNodes.empty() && "Metadata-level alias tree is corrupted!");
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
    if (M.isOriginal() || M.emptyBinding())
      continue;
    findBoundAliasNodes(M, AT, BoundNodes);
    assert(BoundNodes.size() == 1 &&
      "Metadata-level alias tree is corrupted!");
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
trait::DIReduction::ReductionKind getReductionKind(
  const RecurrenceDescriptor &RD) {
  switch (const_cast<RecurrenceDescriptor &>(RD).getRecurrenceKind()) {
  case RecurrenceDescriptor::RK_IntegerAdd:
  case RecurrenceDescriptor::RK_FloatAdd:
    return trait::DIReduction::RK_Add;
  case RecurrenceDescriptor::RK_IntegerMult:
  case RecurrenceDescriptor::RK_FloatMult:
    return trait::DIReduction::RK_Mult;
  case RecurrenceDescriptor::RK_IntegerOr:
    return trait::DIReduction::RK_Or;
  case RecurrenceDescriptor::RK_IntegerAnd:
    return trait::DIReduction::RK_And;
  case RecurrenceDescriptor::RK_IntegerXor:
    return trait::DIReduction::RK_Xor;
  case RecurrenceDescriptor::RK_IntegerMinMax:
  case RecurrenceDescriptor::RK_FloatMinMax:
    switch (const_cast<RecurrenceDescriptor &>(RD).getMinMaxRecurrenceKind()) {
    case RecurrenceDescriptor::MRK_FloatMax:
    case RecurrenceDescriptor::MRK_SIntMax:
    case RecurrenceDescriptor::MRK_UIntMax:
      return trait::DIReduction::RK_Max;
    case RecurrenceDescriptor::MRK_FloatMin:
    case RecurrenceDescriptor::MRK_SIntMin:
    case RecurrenceDescriptor::MRK_UIntMin:
      return trait::DIReduction::RK_Min;
    }
    break;
  }
  llvm_unreachable("Unknown kind of reduction!");
  return trait::DIReduction::RK_NoReduction;
}

/// Update traits of metadata-level locations related to a specified Phi-node
/// in a specified loop. This function uses a specified `TraitInserter` functor
/// to update traits for a single memory location.
template<class FuncT> void updateTraits(const Loop *L, const PHINode *Phi,
    const DominatorTree &DT, Optional<unsigned> DWLang,
    ArrayRef<const DIMemory *> LockedTraits,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    DIMemoryTraitRegionPool &Pool, FuncT &&TraitInserter) {
  for (const auto &Incoming : Phi->incoming_values()) {
    if (!L->contains(Phi->getIncomingBlock(Incoming)))
      continue;
    LLVM_DEBUG(dbgs() << "[DA DI]: traits for promoted location ";
      printLocationSource(dbgs(), Incoming, &DT); dbgs() << " found \n");
    SmallVector<DIMemoryLocation, 2> DILocs;
    Instruction * Users[] = { &Phi->getIncomingBlock(Incoming)->back() };
    findMetadata(Incoming, Users, DT, DILocs);
    if (DILocs.empty())
      continue;
    for (auto &DILoc : DILocs) {
      auto *MD = getRawDIMemoryIfExists(Incoming->getContext(), DILoc);
      if (!MD)
        continue;
      auto DIMTraitItr = Pool.find_as(MD);
      if (DIMTraitItr == Pool.end() ||
          DIMTraitItr->getMemory()->isOriginal() ||
          !DIMTraitItr->getMemory()->emptyBinding() ||
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
    assert(!DIMTraitItr->is<BCL_JOIN(trait::Redundant, trait::NoRedundant>()) &&
      "Conflict in traits for a memory location!");
    if (!(DIMTraitItr->is<trait::Redundant>() &&
          DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode()))
      DIATrait.unset<trait::Redundant>();
    if (IgnoreRedundant && DIMTraitItr->is<trait::Redundant>()) {
      DIATrait.set<trait::NoAccess>();
      DIATrait.unset<trait::HeaderAccess, trait::AddressAccess>();
    }
    return;
  }
  BitMemoryTrait CombinedTrait;
  bool ExplicitAccess = false, Redundant = false, NoRedundant = false;
  for (auto &DIMTraitItr : DIATrait) {
    if (DIMTraitItr->is<trait::ExplicitAccess>() &&
        !DIMTraitItr->is<trait::NoAccess>() &&
        DIATrait.getNode() == DIMTraitItr->getMemory()->getAliasNode())
      ExplicitAccess = true;
    assert(!DIMTraitItr->is<BCL_JOIN(trait::Redundant, trait::NoRedundant>()) &&
      "Conflict in traits for a memory location!");
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
    CombinedTrait &= *DIMTraitItr;
  }
  CombinedTrait &=
    dropUnitFlag(CombinedTrait) == BitMemoryTrait::Readonly ?
      BitMemoryTrait::Readonly :
        dropUnitFlag(CombinedTrait) == BitMemoryTrait::Shared ?
          BitMemoryTrait::Shared : BitMemoryTrait::Dependency;
  auto Dptr = CombinedTrait.toDescriptor(0, NumTraits);
  if (!ExplicitAccess)
    Dptr.unset<trait::ExplicitAccess>();
  if (!Redundant)
    Dptr.unset<trait::Redundant>();
  if (!NoRedundant)
    Dptr.unset<trait::NoRedundant>();
  DIATrait = Dptr;
  LLVM_DEBUG(dbgs() << "[DA DI]: set combined trait to ";
    Dptr.print(dbgs()); dbgs() << "\n");
}

/// Check that a specified corrupted memory location `M` is redundant.
bool isRedundantCorrupted(const DIMemory &M, const AliasTrait &ATrait,
    const AliasTree &AT) {
  if (M.emptyBinding())
    return true;
  auto &VH = *M.begin();
  assert(VH && !isa<UndefValue>(VH) &&
    "Metadata-level alias tree is corrupted!");
  if (auto *DIEM = dyn_cast<DIEstimateMemory>(&M)) {
    auto EM = AT.find(MemoryLocation(VH, DIEM->getSize()));
    if (!EM)
      return true;
    auto MTraitItr = ATrait.find(EM);
    if (MTraitItr == ATrait.end())
      return true;
  } else if (cast<DIUnknownMemory>(M).isExec()) {
      auto MTraitItr = ATrait.find(cast<Instruction>(VH));
      if (MTraitItr == ATrait.unknown_end())
        return true;
  } else {
    for (auto &T : ATrait) {
      auto Itr = std::find(T.getMemory()->begin(), T.getMemory()->end(), VH);
      if (Itr != T.getMemory()->end())
        return false;
    }
    return true;
  }
  return false;
}
}

void DIDependencyAnalysisPass::analyzePromoted(Loop *L,
    Optional<unsigned> DWLang,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const DIMemory *> LockedTraits, DIMemoryTraitRegionPool &Pool) {
  assert(L && "Loop must not be null!");
  // If there is no preheader induction and reduction analysis will fail.
  if (!L->getLoopPreheader())
    return;
  BasicBlock *Header = L->getHeader();
  Function &F = *Header->getParent();
  // Enable analysis of reductions in case of real variables.
  bool HasFunNoNaNAttr =
    F.getFnAttribute("no-nans-fp-math").getValueAsString() == "true";
  if (!HasFunNoNaNAttr)
    F.addFnAttr("no-nans-fp-math", "true");
  for (auto I = L->getHeader()->begin(); isa<PHINode>(I); ++I) {
    auto *Phi = cast<PHINode>(I);
    RecurrenceDescriptor RD;
    InductionDescriptor ID;
    PredicatedScalarEvolution PSE(*mSE, *L);
    if (RecurrenceDescriptor::isReductionPHI(Phi, L, RD)) {
      auto RK = getReductionKind(RD);
      updateTraits(L, Phi, *mDT, DWLang, LockedTraits, DIAliasSTR, Pool,
          [RK](DIMemoryTrait &T) {
        T.set<trait::Reduction>(new trait::DIReduction(RK));
        LLVM_DEBUG(dbgs() << "[DA DI]: reduction found\n");
        ++NumTraits.get<trait::Reduction>();
      });
    } else if (InductionDescriptor::isInductionPHI(Phi, L, PSE, ID)) {
      trait::DIInduction::Constant Start, Step, BackedgeCount;
      if (auto *C = dyn_cast<SCEVConstant>(mSE->getSCEV(ID.getStartValue())))
        Start = APSInt(C->getAPInt());
      if (auto *C = dyn_cast<SCEVConstant>(ID.getStep()))
        Step = APSInt(C->getAPInt());
      if (Start && Step && mSE->hasLoopInvariantBackedgeTakenCount(L))
        if (auto *C = dyn_cast<SCEVConstant>(mSE->getBackedgeTakenCount(L)))
          BackedgeCount = APSInt(C->getAPInt());
      updateTraits(L, Phi, *mDT, DWLang, LockedTraits, DIAliasSTR, Pool,
          [&ID, &Start, &Step, &BackedgeCount](DIMemoryTrait &T) {
        auto DIEM = dyn_cast<DIEstimateMemory>(T.getMemory());
        if (!DIEM)
          return;
        SourceUnparserImp Unparser(DIMemoryLocation(
          const_cast<DIVariable *>(DIEM->getVariable()),
          const_cast<DIExpression *>(DIEM->getExpression())),
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
    }
  }
  if (!HasFunNoNaNAttr)
    F.addFnAttr("no-nans-fp-math", "false");
}

void DIDependencyAnalysisPass::analyzeNode(DIAliasMemoryNode &DIN,
    Optional<unsigned> DWLang,
    const SpanningTreeRelation<AliasTree *> &AliasSTR,
    const SpanningTreeRelation<const tsar::DIAliasTree *> &DIAliasSTR,
    ArrayRef<const DIMemory *> LockedTraits, const GlobalOptions &GlobalOpts,
    DependenceSet &DepSet, DIDependenceSet &DIDepSet,
    DIMemoryTraitRegionPool &Pool) {
  assert(!DIN.empty() && "Alias node must contain memory locations!");
  auto *AN = findBoundAliasNode(*mAT, AliasSTR, DIN);
  auto ATraitItr = AN ? DepSet.find_as(AN) : DepSet.end();
  DIDependenceSet::iterator DIATraitItr = DIDepSet.end();
  for (auto &M : DIN) {
    LLVM_DEBUG(if (DWLang) {
      dbgs() << "[DA DI]: extract traits for ";
      printDILocationSource(*DWLang, M, dbgs());
      dbgs() << "\n";
    });
    if (M.isOriginal() || M.emptyBinding()) {
      auto DIMTraitItr = Pool.find_as(&M);
      if (DIMTraitItr == Pool.end())
        continue;
      LLVM_DEBUG(dbgs() << "[DA DI]: use existent traits\n");
      if (M.isOriginal() &&
          !isLockedTrait(*DIMTraitItr, LockedTraits, DIAliasSTR) &&
          (ATraitItr == DepSet.end() ||
           isRedundantCorrupted(M, *ATraitItr, *mAT))) {
        DIMTraitItr->set<trait::Redundant>();
        DIMTraitItr->unset<trait::NoRedundant>();
      }
      if (DIATraitItr == DIDepSet.end())
        DIATraitItr = DIDepSet.insert(DIAliasTrait(&DIN)).first;
      DIATraitItr->insert(DIMTraitItr);
      continue;
    }
    // If memory from this metadata alias node is not accessed in the region
    // then do not process memory from this node. Note, that some corrupted
    // memory covered by this metadata-level node may be accessed in the region.
    // So, exit from loop but not from a function.
    if (ATraitItr == DepSet.end())
      break;
    auto &VH = *M.begin();
    assert(VH && !isa<UndefValue>(VH) &&
      "Metadata-level alias tree is corrupted!");
    DIMemoryTraitRef DIMTraitItr;
    if (auto *DIEM = dyn_cast<DIEstimateMemory>(&M)) {
      auto EM = mAT->find(MemoryLocation(VH, DIEM->getSize()));
      assert(EM && "Estimate memory must be presented in the alias tree!");
      auto MTraitItr = ATraitItr->find(EM);
      // If memory location is not explicitly accessed in the region and if it
      // does not cover any explicitly accessed location then go to the next
      // location.
      if (MTraitItr == ATraitItr->end())
        continue;
      bool IsNotLocked = false;
      std::tie(DIMTraitItr, IsNotLocked) =
        addOrUpdateInPool(M, *MTraitItr, LockedTraits, DIAliasSTR, Pool);
      if (IsNotLocked) {
        convertIf<trait::Flow>(*MTraitItr, *DIMTraitItr);
        convertIf<trait::Anti>(*MTraitItr, *DIMTraitItr);
        convertIf<trait::Output>(*MTraitItr, *DIMTraitItr);
      }
    } else if (cast<DIUnknownMemory>(M).isExec()) {
      auto MTraitItr = ATraitItr->find(cast<Instruction>(VH));
      // If memory location is not explicitly accessed in the region and if it
      // does not cover any explicitly accessed location then go to the next
      // location.
      if (MTraitItr == ATraitItr->unknown_end())
        continue;
      std::tie(DIMTraitItr, std::ignore) =
        addOrUpdateInPool(M, *MTraitItr, LockedTraits, DIAliasSTR, Pool);
    } else {
      bool IsTraitFound = false;
      for (auto &T : *ATraitItr) {
        auto Itr = std::find(T.getMemory()->begin(), T.getMemory()->end(), VH);
        if (Itr != T.getMemory()->end()) {
          bool IsNotLocked = false;
          std::tie(DIMTraitItr, IsNotLocked) =
            addOrUpdateInPool(M, T, LockedTraits, DIAliasSTR, Pool);
          if (IsNotLocked) {
            convertIf<trait::Flow>(T, *DIMTraitItr);
            convertIf<trait::Anti>(T, *DIMTraitItr);
            convertIf<trait::Output>(T, *DIMTraitItr);
          }
          IsTraitFound = true;
        }
      }
      // If memory location is not explicitly accessed in the region and if it
      // does not cover any explicitly accessed location then go to the next
      // location.
      if (!IsTraitFound)
        continue;
    }
    if (DIATraitItr == DIDepSet.end())
      DIATraitItr = DIDepSet.insert(DIAliasTrait(&DIN)).first;
    DIATraitItr->insert(DIMTraitItr);
  }
  // Collect locations from descendant nodes which are explicitly accessed.
  for (auto &Child : make_range(DIN.child_begin(), DIN.child_end())) {
    auto ChildTraitItr = DIDepSet.find_as(&Child);
    // If memory from this child is not accessed in a region then go to the next
    // child.
    if (ChildTraitItr == DIDepSet.end())
      continue;
    for (auto &DIMTraitItr : *ChildTraitItr) {
      auto *DIM = DIMTraitItr->getMemory();
      // Alias trait contains not only locations from a corresponding alias
      // node. It may also contain locations from descendant nodes.
      auto N = DIM->getAliasNode();
      // Locations from descendant estimate nodes have been already processed
      // implicitly when IR-level analysis has been performed and traits for
      // estimate memory locations have been fetched for nodes in IR-level
      // alias tree.
      if (isa<DIAliasEstimateNode>(DIN) &&
          !DIM->emptyBinding() && isa<DIAliasEstimateNode>(N))
        continue;
      LLVM_DEBUG(if (DWLang) {
        dbgs() << "[DA DI]: extract traits from descendant node";
        printDILocationSource(*DWLang, *DIM, dbgs());
        dbgs() << "\n";
      });
      if (DIATraitItr == DIDepSet.end())
        DIATraitItr = DIDepSet.insert(DIAliasTrait(&DIN)).first;
      DIATraitItr->insert(DIMTraitItr);
    }
  }
  if (DIATraitItr == DIDepSet.end())
    return;
  combineTraits(GlobalOpts.IgnoreRedundantMemory, *DIATraitItr);
  // We do not update traits for each memory location (as in private recognition
  // pass) because these traits should be updated early (during analysis of
  // promoted locations or by the private recognition pass).
}


bool DIDependencyAnalysisPass::runOnFunction(Function &F) {
  releaseMemory();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  if (!GlobalOpts.AnalyzeLibFunc && hasFnAttr(F, AttrKind::LibFunc))
    return false;
  mDT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  mAT = &getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &PI = getAnalysis<PrivateRecognitionPass>().getPrivateInfo();
  auto &DIAT = getAnalysis<DIEstimateMemoryPass>().getAliasTree();
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto &DL = F.getParent()->getDataLayout();
  auto DWLang = getLanguage(F);
  SpanningTreeRelation<AliasTree *> AliasSTR(mAT);
  SpanningTreeRelation<const DIAliasTree *> DIAliasSTR(&DIAT);
  for (auto &Info : PI) {
    if (!isa<DFLoop>(Info.get<DFNode>()))
      continue;
    auto L = cast<DFLoop>(Info.get<DFNode>())->getLoop();
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
    auto &Pool = TraitPool[DILoop];
    LLVM_DEBUG(if (DWLang) allocatePoolLog(*DWLang, Pool));
    SmallVector<const DIMemory *, 4> LockedTraits;
    if (!Pool) {
      Pool = make_unique<DIMemoryTraitRegionPool>();
    } else {
      for (auto &T : *Pool)
        if (T.is<trait::Lock>())
          LockedTraits.push_back(T.getMemory());
    }
    auto &DepSet = Info.get<DependenceSet>();
    auto &DIDepSet = mDeps.try_emplace(DILoop, DepSet.size()).first->second;
    analyzePromoted(L, DWLang, DIAliasSTR, LockedTraits, *Pool);
    for (auto *DIN : post_order(&DIAT)) {
      if (isa<DIAliasTopNode>(DIN))
        continue;
      analyzeNode(cast<DIAliasMemoryNode>(*DIN), DWLang, AliasSTR, DIAliasSTR,
        LockedTraits, GlobalOpts, DepSet, DIDepSet, *Pool);
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
/// If TraitT is set and its tag is presented in SeparateTraitListT then store
/// a specified value in a list of traits.
template<class TraitT, class ValueT, class SeparateTraitListT>
struct StoreIfSet {
  template<class Trait> void operator()() {
    if (T.template is<Trait>())
      Traits.template get<Trait>().insert(V);
  }

  const TraitT &T;
  const ValueT &V;
  SeparateTraitListT &Traits;
};

template<class TraitT, class ValueT, class SeparateTraitListT>
StoreIfSet<TraitT, ValueT, SeparateTraitListT> getStoreIfSetFunctor(
    const TraitT &T, const ValueT &V, SeparateTraitListT &Traits) {
  return StoreIfSet<TraitT, ValueT, SeparateTraitListT>{ T, V, Traits};
}

template<class... SeparateTraits>
struct TraitPrinter {
  /// Sorted list of traits to (print their in algoristic order).
  using SortedVarListT = std::set<std::string, std::less<std::string>>;

  /// Container of traits which should be printed if they have been set for
  /// a memory location separately (it may not be set for the whole alias node).
  using SeparateTraitList =
    bcl::tags::get_tagged_tuple_t<SortedVarListT, SeparateTraits...>;

  /// Functor to print some traits for memory locations separately.
  struct SeparatePrinter {
    template<class TagT> void operator()() {
      if (Traits.template get<TagT>().empty())
        return;
      OS << Offset << TagT::toString() << " (separate):\n" << Offset;
      for (auto &T : Traits.template get<TagT>())
        OS << " " << T;
      OS << "\n";
    }
    SeparateTraitList &Traits;
    std::string Offset;
    llvm::raw_ostream &OS;
  };

  explicit TraitPrinter(raw_ostream &OS, const DIAliasTree &DIAT,
    StringRef Offset, unsigned DWLang) :
    mOS(OS), mDIAT(&DIAT), mOffset(Offset), mDWLang(DWLang) {}

  /// Print traits from `SeparateTraits` if they have been set for
  /// a memory location separately (it may not be set for the whole alias node).
  void printSeparateTraits() {
    bcl::TypeList<SeparateTraits...>::template for_each_type(
      SeparatePrinter{ mSeparateTraits, mOffset, mOS });
  }

  template<class Trait> void operator()(
      ArrayRef<const DIAliasTrait *> TraitVector) {
    if (TraitVector.empty() || std::is_same<Trait, trait::NoRedundant>())
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
        if (!std::is_same<Trait, trait::AddressAccess>::value &&
            T->is<trait::NoAccess>() ||
            std::is_same<Trait, trait::AddressAccess>::value && !T->is<Trait>())
          continue;
        std::string Str;
        raw_string_ostream TmpOS(Str);
        printDILocationSource(mDWLang, *T->getMemory(), TmpOS);
        bcl::TypeList<SeparateTraits...>::template for_each_type(
          getStoreIfSetFunctor(*T, TmpOS.str(), mSeparateTraits));
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
    if (!Dep)
      return;
    if (!Dep->getDistance().first && !Dep->getDistance().second)
      return;
    OS << ":[";
    if (Dep->getDistance().first)
      OS << *Dep->getDistance().first;
    OS << ",";
    if (Dep->getDistance().second)
      OS << *Dep->getDistance().second;
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
  SeparateTraitList mSeparateTraits;
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
    M->getContext().emitError(
      "unknown source language for function " + DIAT.getFunction().getName());
    return;
  }
  for_each_loop(LpInfo, [this, M, &GlobalOpts, &DIAT, &OS, &DWLang](Loop *L) {
    DebugLoc Loc = L->getStartLoc();
    std::string Offset(L->getLoopDepth(), ' ');
    OS << Offset;
    OS << "loop at depth " << L->getLoopDepth() << " ";
    tsar::print(OS, Loc, GlobalOpts.PrintFilenameOnly);
    OS << "\n";
    Offset.append("  ");
    auto DILoop = L->getLoopID();
    if (!DILoop) {
      M->getContext().emitError("loop has not been analyzed"
        " due to absence of debug information");
      return;
    }
    auto Info = mDeps.find(DILoop);
    assert(Info != mDeps.end() && "Results of analysis are not found!");
    using TraitMap = bcl::StaticTraitMap<
      std::vector<const DIAliasTrait *>, MemoryDescriptor>;
    TraitMap TM;
    DenseSet<const DIAliasNode *> Coverage;
    accessCoverage<bcl::SimpleInserter>(Info->get<DIDependenceSet> (),
      DIAT, Coverage, GlobalOpts.IgnoreRedundantMemory);
    for (auto &TS : Info->get<DIDependenceSet>()) {
      if (Coverage.count(TS.getNode()))
        TS.for_each(
          bcl::TraitMapConstructor<const DIAliasTrait, TraitMap>(TS, TM));
      else if (TS.is<trait::Redundant>())
        TM.value<trait::Redundant>().push_back(&TS);
    }
    TraitPrinter<trait::ExplicitAccess, trait::Redundant>
      Printer(OS, DIAT, Offset, *DWLang);
    TM.for_each(Printer);
    Printer.printSeparateTraits();
  });
}

void DIDependencyAnalysisPass::getAnalysisUsage(AnalysisUsage &AU)  const {
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<EstimateMemoryPass>();
  AU.addRequired<DIEstimateMemoryPass>();
  AU.addRequired<PrivateRecognitionPass>();
  AU.addRequired<DIMemoryTraitPoolWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.setPreservesAll();
}

FunctionPass * llvm::createDIDependencyAnalysisPass() {
  return new DIDependencyAnalysisPass();
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
  assert((mPool->find_as(M) == mPool->end() ||
    mPool->find_as(M)->getMemory() != M) &&
    "New memory location is already presented in the memory trait pool!");
  DIMemoryTraitRegionPool::persistent_iterator OldItr =
    mPool->find_as(getMemoryPtr());
  LLVM_DEBUG(
    dbgs() << "[DA DI]: replace in pool metadata-level memory location ";
    printDILocationSource(dwarf::DW_LANG_C, *getMemoryPtr(), dbgs());
    dbgs() << " with ";
    printDILocationSource(dwarf::DW_LANG_C, *M, dbgs());
    dbgs() << "\n");
  auto TS(std::move(OldItr->getSecond()));
  auto Pool = mPool;
  // Do not use members of handle after the call of erase(), because
  // it destroys this.
  Pool->erase(OldItr);
  Pool->try_emplace({ M, Pool }, std::move(TS));
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
