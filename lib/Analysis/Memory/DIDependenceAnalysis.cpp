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
#include "tsar_dbg_output.h"
#include "DIEstimateMemory.h"
#include "EstimateMemory.h"
#include "GlobalOptions.h"
#include "MemoryAccessUtils.h"
#include "tsar_query.h"
#include "SourceUnparser.h"
#include "tsar/Analysis/Memory/PrivateAnalysis.h"
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Support/Debug.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/Transforms/Utils/LoopUtils.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "da-di"

MEMORY_TRAIT_STATISTIC(NumTraits)

STATISTIC(NumLoops, "Number of loops analyzed");
STATISTIC(NumPrivateLoops, "Number of loops with private locations found");
STATISTIC(NumLPrivateLoops, "Number of loops with last private locations found");
STATISTIC(NumSToLPrivateLoops, "Number of loops with second to last private locations found");
STATISTIC(NumFPrivateLoops, "Number of loops with first private locations found");
STATISTIC(NumDPrivateLoops, "Number of loops with dynamic private locations found");
STATISTIC(NumDepLoops, "Number of loops with unsorted dependencies found");
STATISTIC(NumSharedLoops, "Number of loops with shared locations found");
STATISTIC(NumInductionLoops, "Number of loops with induction locations found");
STATISTIC(NumReductionLoops, "Number of loops with reduction locations found");


char DIDependencyAnalysisPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(DIDependencyAnalysisPass, "da-di",
  "Dependency Analysis (Metadata)", false, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(LoopPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
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

/// Returns number of explicitly accessed locations in the dependence set.
std::size_t accessedMemoryCount(AliasTree &AT, DependencySet &DS) {
  auto TopTraitsItr = DS.find(AT.getTopLevelNode());
  return TopTraitsItr->count() + TopTraitsItr->unknown_count();
}

/// \brief Converts IR-level representation of a dependence of
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

void convertTraitsForEstimateNode(DIAliasEstimateNode &DIN, AliasTree &AT,
    DependencySet &DepSet, DIDependenceSet &DIDepSet,
    DIMemoryTraitRegionPool &Pool) {
  DependencySet::iterator ATraitItr = DepSet.end();
  for (auto &Mem : DIN) {
    auto &M = cast<DIEstimateMemory>(Mem);
    if (M.emptyBinding())
      continue;
    /// TODO (kaniandr@gmail.com): use variable to specify language.
    LLVM_DEBUG(dbgs() << "[DA DI]: extract traits for ";
    printDILocationSource(dwarf::DW_LANG_C, M, dbgs()); dbgs() << "\n");
    auto &VH = *M.begin();
    // Conditions which are checked bellow may be violated if DIAliasTree
    // has not been rebuild after transformations.
    assert(VH && !isa<UndefValue>(VH) &&
      "Metadata-level alias tree is corrupted!");
    auto EM = AT.find(MemoryLocation(VH, M.getSize()));
    assert(EM && "Estimate memory must be presented in the alias tree!");
    auto AN = EM->getAliasNode(AT);
    assert((ATraitItr == DepSet.end() || ATraitItr->getNode() == AN) &&
      "Multiple AliasNodes associated with a DIEstimateMemoryNode!");
    if (ATraitItr == DepSet.end())
      ATraitItr = DepSet.find(AN);
    // If memory from this metadata alias node does not accessed in the region
    // then go to the next node.
    if (ATraitItr == DepSet.end())
      return;
    auto MTraitItr = ATraitItr->find(EM);
    // If memory location is not explicitly accessed in the region and if it
    // does not cover any explicitly accessed location then go to the next
    // location.
    if (MTraitItr == ATraitItr->end())
      continue;
    auto DIMTraitItr = Pool.find_as(&M);
    LLVM_DEBUG(if (DIMTraitItr == Pool.end())
      dbgs() << "[DA DI]: add new trait to pool\n");
    if (DIMTraitItr != Pool.end())
      *DIMTraitItr = *MTraitItr;
    else
      DIMTraitItr = Pool.try_emplace({ &M, &Pool }, *MTraitItr).first;
    convertIf<trait::Flow>(*MTraitItr, *DIMTraitItr);
    convertIf<trait::Anti>(*MTraitItr, *DIMTraitItr);
    convertIf<trait::Output>(*MTraitItr, *DIMTraitItr);
    /// TODO (kaninadr@gmail.com): merge traits
    auto DIATraitPair = DIDepSet.insert(DIAliasTrait(&DIN, *DIMTraitItr));
    if (!DIATraitPair.second) {
      DIATraitPair.first->set<trait::Flow, trait::Anti, trait::Output>();
    }
    DIATraitPair.first->insert(DIMTraitItr);
  }
  // At first we evaluate locations with bounded memory. If there are such
  // locations, however, there is no traits for this locations, this means
  // that this memory does not accessed in this loop. So, processing will be
  // ended (see return in the previous loop) before running this loop.
  for (auto &Mem : DIN) {
    auto &M = cast<DIEstimateMemory>(Mem);
    if (!M.emptyBinding())
      continue;
    /// TODO (kaniandr@gmail.com): use variable to specify language.
    LLVM_DEBUG(dbgs() << "[DA DI]: extract traits for ";
    printDILocationSource(dwarf::DW_LANG_C, M, dbgs()); dbgs() << "\n");
    auto DIMTraitItr = Pool.find_as(&M);
    if (DIMTraitItr == Pool.end())
      continue;
    LLVM_DEBUG(dbgs() << "[DA DI]: use existent traits\n");
    /// TODO (kaninadr@gmail.com): merge traits
    auto DIATraitPair = DIDepSet.insert(DIAliasTrait(&DIN, *DIMTraitItr));
    if (!DIATraitPair.second) {
      DIATraitPair.first->set<trait::Flow, trait::Anti, trait::Output>();
    }
    DIATraitPair.first->insert(DIMTraitItr);
  }
}
}

void DIDependencyAnalysisPass::analyzePromoted(Loop *L,
    Optional<unsigned> DWLang, DIMemoryTraitRegionPool &Pool) {
  // If there is no preheader induction and reduction analysis will fail.
  if (!L->getLoopPreheader())
    return;
  for (auto I = L->getHeader()->begin(); isa<PHINode>(I); ++I) {
    auto *Phi = cast<PHINode>(I);
    InductionDescriptor ID;
    RecurrenceDescriptor RD;
    PredicatedScalarEvolution PSE(*mSE, *L);
    BasicBlock *Header = L->getHeader();
    Function &F = *Header->getParent();
    // Enable analysis of reductions in case of real variables.
    bool HasFunNoNaNAttr =
      F.getFnAttribute("no-nans-fp-math").getValueAsString() == "true";
    if (!HasFunNoNaNAttr)
      F.addFnAttr("no-nans-fp-math", "true");
    if (RecurrenceDescriptor::isReductionPHI(Phi, L, RD)) {
      ++NumReductionLoops;
      ++NumTraits.get<trait::Reduction>();
    } else if (InductionDescriptor::isInductionPHI(Phi, L, PSE, ID)) {
      ++NumInductionLoops;
      ++NumTraits.get<trait::Induction>();
    }
    if (!HasFunNoNaNAttr)
      F.addFnAttr("no-nans-fp-math", "false");
    trait::DIReduction Red(trait::DIReduction::RK_NoReduction);
    trait::DIInduction Ind(trait::DIInduction::InductionKind::IK_NoInduction);
    if (RD.getRecurrenceKind() != RecurrenceDescriptor::RK_NoRecurrence) {
      auto RK = trait::DIReduction::RK_NoReduction;
      switch (RD.getRecurrenceKind()) {
      case RecurrenceDescriptor::RK_IntegerAdd:
      case RecurrenceDescriptor::RK_FloatAdd:
        RK = trait::DIReduction::RK_Add; break;
      case RecurrenceDescriptor::RK_IntegerMult:
      case RecurrenceDescriptor::RK_FloatMult:
        RK = trait::DIReduction::RK_Mult; break;
      case RecurrenceDescriptor::RK_IntegerOr:
        RK = trait::DIReduction::RK_Or; break;
      case RecurrenceDescriptor::RK_IntegerAnd:
        RK = trait::DIReduction::RK_And; break;
      case RecurrenceDescriptor::RK_IntegerXor:
        RK = trait::DIReduction::RK_Xor; break;
      case RecurrenceDescriptor::RK_IntegerMinMax:
      case RecurrenceDescriptor::RK_FloatMinMax:
        switch (RD.getMinMaxRecurrenceKind()) {
        case RecurrenceDescriptor::MRK_FloatMax:
        case RecurrenceDescriptor::MRK_SIntMax:
        case RecurrenceDescriptor::MRK_UIntMax:
          RK = trait::DIReduction::RK_Max; break;
        case RecurrenceDescriptor::MRK_FloatMin:
        case RecurrenceDescriptor::MRK_SIntMin:
        case RecurrenceDescriptor::MRK_UIntMin:
          RK = trait::DIReduction::RK_Min; break;
        }
        break;
      }
      assert(RK != trait::DIReduction::RK_NoReduction &&
        "Unknown kind of reduction!");
      Red = trait::DIReduction(RK);
    } else if (ID.getKind() != InductionDescriptor::IK_NoInduction) {
      trait::DIInduction::Constant Start, Step, End;
      if (auto StartConst =
          dyn_cast<SCEVConstant>(mSE->getSCEV(ID.getStartValue())))
        Start = APSInt(StartConst->getAPInt());
      if (auto StepConst = dyn_cast<SCEVConstant>(ID.getStep()))
        Step = APSInt(StepConst->getAPInt());
      if (Start && Step && mSE->hasLoopInvariantBackedgeTakenCount(L))
        if (auto BackedgeConst =
            dyn_cast<SCEVConstant>(mSE->getBackedgeTakenCount(L)))
          End = APSInt(BackedgeConst->getAPInt());
      // We temporary store back edge count instead of upper bound of induction.
      // So, to store trait in a trait set it should be recalculated.
      Ind = trait::DIInduction(ID.getKind(), Start, End, Step);
    } else {
      continue;
    }
    for (const auto &Incoming : Phi->incoming_values()) {
      if (!L->contains(Phi->getIncomingBlock(Incoming)))
        continue;
      LLVM_DEBUG(
        dbgs() << "[DA DI]: in loop at ";
        auto LpLoc = L->getStartLoc();
        if (LpLoc)
          LpLoc.print(dbgs());
        else
          dbgs() << "unknown location";
        dbgs() <<
          (ID.getKind() != InductionDescriptor::IK_NoInduction ? " induction" :
          RD.getRecurrenceKind() != RecurrenceDescriptor::RK_NoRecurrence ?
            " reduction" : "unknown") << " trait found for ";
        printLocationSource(dbgs(), Incoming, mDT); dbgs() << "\n");
      SmallVector<DIMemoryLocation, 2> DILocs;
      findMetadata(Incoming, DILocs, mDT);
      if (DILocs.empty())
        continue;
      for (auto &DILoc : DILocs) {
        auto *MD = getRawDIMemoryIfExists(Incoming->getContext(), DILoc);
        if (!MD)
          continue;
        auto DIMTraitItr = Pool.find_as(MD);
        if (DIMTraitItr == Pool.end() ||
            !DIMTraitItr->getMemory()->emptyBinding() ||
            !DIMTraitItr->is<trait::Anti, trait::Flow, trait::Output>())
          continue;
        LLVM_DEBUG(if (DWLang) {
          dbgs() << "[DA DI]: update traits for ";
          printDILocationSource(*DWLang, *DIMTraitItr->getMemory(), dbgs());
          dbgs() << "\n";
        });
        if (Red)
          DIMTraitItr->set<trait::Reduction>(new trait::DIReduction(Red));
        else if (Ind) {
          auto Start = Ind.getStart();
          auto End = Ind.getEnd();
          auto Step = Ind.getStep();
          if (auto DIEM = dyn_cast<DIEstimateMemory>(DIMTraitItr->getMemory())) {
            SourceUnparserImp Unparser(
              DIMemoryLocation(const_cast<DIVariable *>(DIEM->getVariable()),
                const_cast<DIExpression *>(DIEM->getExpression())),
              true /*order of dimensions is not important here*/);
            if (Unparser.unparse() && !Unparser.getIdentifiers().empty()) {
              auto Id = Unparser.getIdentifiers().back();
              assert(Id && "Identifier must not be null!");
              DIType *DITy = nullptr;
              if (auto DIVar = dyn_cast<DIVariable>(Id))
                DITy = DIVar->getType().resolve();
              else
                DITy = dyn_cast<DIDerivedType>(Id);
              while (DITy && isa<DIDerivedType>(DITy))
                DITy = cast<DIDerivedType>(DITy)->getBaseType().resolve();
              if (auto DIBasicTy = dyn_cast_or_null<DIBasicType>(DITy))
                if (DIBasicTy->getEncoding() == dwarf::DW_ATE_signed) {
                  if (Start)
                    Start->setIsSigned(true);
                  if (End)
                    End->setIsSigned(true);
                  if (Step)
                    Step->setIsSigned(true);
                  if (Start && Step && End)
                    End = *Start + *End * *Step;
                  else
                    End.reset();
                  DIMTraitItr->set<trait::Induction>(
                    new trait::DIInduction(Ind.getKind(), Start, End, Step));
                  continue;
                } else if (DIBasicTy->getEncoding() == dwarf::DW_ATE_unsigned) {
                  if (Start)
                    Start->setIsSigned(false);
                  if (End)
                    End->setIsSigned(false);
                  if (Step)
                    Step->setIsSigned(false);
                  if (Start && Step && End)
                    End = *Start + *End * *Step;
                  else
                    End.reset();
                  DIMTraitItr->set<trait::Induction>(
                    new trait::DIInduction(Ind.getKind(), Start, End, Step));
                  continue;
                }
            }
          }
          DIMTraitItr->set<trait::Induction>(
            new trait::DIInduction(Ind.getKind()));
        }
      }
    }
  }
}

bool DIDependencyAnalysisPass::runOnFunction(Function &F) {
  mDT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  mSE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &LI = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &AT = getAnalysis<EstimateMemoryPass>().getAliasTree();
  auto &PI = getAnalysis<PrivateRecognitionPass>().getPrivateInfo();
  auto &DIAT = getAnalysis<DIEstimateMemoryPass>().getAliasTree();
  auto &TraitPool = getAnalysis<DIMemoryTraitPoolWrapper>().get();
  auto &DL = F.getParent()->getDataLayout();
  auto DWLang = getLanguage(F);
  for (auto &Info : PI) {
    if (!isa<DFLoop>(Info.get<DFNode>()))
      continue;
    auto L = cast<DFLoop>(Info.get<DFNode>())->getLoop();
    if (!L->getLoopID())
      continue;
    assert(L->getLoopID() && "Identifier of a loop must be specified!");
    auto DILoop = L->getLoopID();
    LLVM_DEBUG(dbgs() << "[DA DI]: process loop at ";
               TSAR_LLVM_DUMP(L->getStartLoc().dump()); dbgs() << "\n");
    auto &Pool = TraitPool[DILoop];
    LLVM_DEBUG(if (DWLang) allocatePoolLog(*DWLang, Pool));
    auto &DepSet = *Info.get<DependencySet>();
    if (!Pool)
      Pool = make_unique<DIMemoryTraitRegionPool>();
    auto &DIDepSet = mDeps.try_emplace(DILoop, DepSet.size()).first->second;
    analyzePromoted(L, DWLang, *Pool);
    for (auto *DIN : post_order(&DIAT)) {
      if (isa<DIAliasTopNode>(DIN))
        continue;
      if (auto MemoryNode = dyn_cast<DIAliasEstimateNode>(DIN)) {
        convertTraitsForEstimateNode(*MemoryNode, AT,
          *Info.get<DependencySet>(), DIDepSet, *Pool);
      } else {
        /// TODO (kaniandr@gmail.com): processing of unknown nodes is temporary
        /// too conservative.
        auto UnknownNode = cast<DIAliasUnknownNode>(DIN);
        DIMemoryTraitSet Dptr;
        Dptr.set<trait::Flow, trait::Anti, trait::Output>();
        for (auto &Mem : *UnknownNode) {
          /// TODO (kaniandr@gmail.com): use variable to specify language.
          LLVM_DEBUG(dbgs() << "[DA DI]: extract traits for ";
          printDILocationSource(dwarf::DW_LANG_C, Mem, dbgs()); dbgs() << "\n");
          auto DIMTraitPair = Pool->try_emplace({ &Mem, Pool.get() }, Dptr);
          if (!DIMTraitPair.second)
            continue;
          LLVM_DEBUG(dbgs() << "[DA DI]: use existent traits\n");
          /// TODO (kaninadr@gmail.com): merge traits
          auto DIATraitPair = DIDepSet.insert(DIAliasTrait(DIN, Dptr));
          DIATraitPair.first->insert(DIMTraitPair.first);
        }
      }
    }
  }
  return false;
}

namespace {
struct TraitPrinter {
  explicit TraitPrinter(raw_ostream &OS, const DIAliasTree &DIAT,
    StringRef Offset, unsigned DWLang) :
    mOS(OS), mDIAT(&DIAT), mOffset(Offset), mDWLang(DWLang) {}
  template<class Trait> void operator()(
      ArrayRef<const DIAliasTrait *> TraitVector) {
    if (TraitVector.empty())
      return;
    mOS << mOffset << Trait::toString() << ":\n";
    /// Sort traits to print their in algoristic order.
    using SortedVarListT = std::set<std::string, std::less<std::string>>;
    std::vector<SortedVarListT> VarLists;
    auto less = [&VarLists](decltype(VarLists)::size_type LHS,
      decltype(VarLists)::size_type RHS) {
      return VarLists[LHS] < VarLists[RHS];
    };
    std::set<decltype(VarLists)::size_type, decltype(less)> ANTraitList(less);
    for (auto *AT : TraitVector) {
      if (AT->getNode() == mDIAT->getTopLevelNode())
        continue;
      VarLists.emplace_back();
      for (auto &T : *AT) {
        if (!std::is_same<Trait, trait::AddressAccess>::value &&
            T->is<trait::NoAccess>() ||
            std::is_same<Trait, trait::AddressAccess>::value && !T->is<Trait>())
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
};
}

void DIDependencyAnalysisPass::print(raw_ostream &OS, const Module *M) const {
  auto &LpInfo = getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &DIAT = getAnalysis<DIEstimateMemoryPass>().getAliasTree();
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
    for (auto &TS : Info->get<DIDependenceSet>())
      TS.for_each(
        bcl::TraitMapConstructor<const DIAliasTrait, TraitMap>(TS, TM));
    TM.for_each(TraitPrinter(OS, DIAT, Offset, *DWLang));
  });
}

void DIDependencyAnalysisPass::getAnalysisUsage(AnalysisUsage &AU)  const {
  // Call getLoopAnalysisUsage(AU) at the beginning. Otherwise, pass manager
  // can not schedule passes.
  //getLoopAnalysisUsage(AU);
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
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
