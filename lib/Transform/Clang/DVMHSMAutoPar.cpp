//===-- DVMHSMAutoPar.cpp - OpenMP Based Parallelization (Clang) -*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements a pass to perform DVMH-based auto parallelization for
// shared memory.
//
//===----------------------------------------------------------------------===//

#include "SharedMemoryAutoPar.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/ASTDependenceAnalysis.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Clang/Passes.h"

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-sm-parallel"

namespace {

using ReductionVarListT = ClangDependenceAnalyzer::ReductionVarListT;
using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;
using ASTRegionTraitInfo = ClangDependenceAnalyzer::ASTRegionTraitInfo;
using FunctionAnalysisResult = std::tuple <DominatorTree*, PostDominatorTree*,
  TargetLibraryInfo*, AliasTree*, DIMemoryClientServerInfo*,
  const CanonicalLoopSet*, DFRegionInfo*>;

void addVarList(const ClangDependenceAnalyzer::SortedVarListT &VarInfoList,
    SmallVectorImpl<char> &Clause) {
  Clause.push_back('(');
  auto I = VarInfoList.begin(), EI = VarInfoList.end();
  Clause.append(I->begin(), I->end());
  for (++I; I != EI; ++I) {
    Clause.append({ ',', ' ' });
    Clause.append(I->begin(), I->end());
  }
  Clause.push_back(')');
}

/// Add clauses for all reduction variables from a specified list to
/// the end of `ParallelFor` pragma.
void addVarList(const ClangDependenceAnalyzer::ReductionVarListT &VarInfoList,
    SmallVectorImpl<char> &ParallelFor) {
  unsigned I = trait::Reduction::RK_First;
  unsigned EI = trait::Reduction::RK_NumberOf;
  for (; I < EI; ++I) {
    if (VarInfoList[I].empty())
      continue;
    SmallString<7> RedKind;
    switch (static_cast<trait::Reduction::Kind>(I)) {
    case trait::Reduction::RK_Add: RedKind += "sum"; break;
    case trait::Reduction::RK_Mult: RedKind += "product"; break;
    case trait::Reduction::RK_Or: RedKind += "or"; break;
    case trait::Reduction::RK_And: RedKind += "and"; break;
    case trait::Reduction::RK_Xor: RedKind + "xor "; break;
    case trait::Reduction::RK_Max: RedKind += "max"; break;
    case trait::Reduction::RK_Min: RedKind += "min"; break;
    default: llvm_unreachable("Unknown reduction kind!"); break;
    }
    ParallelFor.append({ 'r', 'e', 'd', 'u', 'c', 't', 'i', 'o', 'n' });
    ParallelFor.push_back('(');
    auto VarItr = VarInfoList[I].begin(), VarItrE = VarInfoList[I].end();
    ParallelFor.append(RedKind.begin(), RedKind.end());
    ParallelFor.push_back('(');
    ParallelFor.append(VarItr->begin(), VarItr->end());
    ParallelFor.push_back(')');
    for (++VarItr; VarItr != VarItrE; ++VarItr) {
      ParallelFor.push_back(',');
      ParallelFor.append(RedKind.begin(), RedKind.end());
      ParallelFor.push_back('(');
      ParallelFor.append(VarItr->begin(), VarItr->end());
      ParallelFor.push_back(')');
    }
    ParallelFor.push_back(')');
  }
}

unsigned getPerfectNestSize(const DFLoop &DFL,
    const PerfectLoopInfo &PerfectInfo,
    const CanonicalLoopSet &CanonicalLoops) {
  auto *CurrDFL = &DFL;
  unsigned PerfectSize = 1;
  for (; PerfectInfo.count(CurrDFL) && CurrDFL->getNumRegions() > 0;
       ++PerfectSize) {
    CurrDFL = dyn_cast<DFLoop>(*CurrDFL->region_begin());
    if (!CurrDFL)
      return PerfectSize;
    auto CanonicalItr = CanonicalLoops.find_as(const_cast<DFLoop *>(CurrDFL));
    if (CanonicalItr == CanonicalLoops.end() || !(**CanonicalItr).isCanonical())
      return PerfectSize;
  }
  return PerfectSize;
}

SortedVarListT mergeVarSets(const std::vector<SortedVarListT>& VarLists) {
  SortedVarListT Result;
  for (auto List : VarLists) {
    for (auto VarName : List) {
      Result.insert(VarName);
    }
  }
  return Result;
}

ReductionVarListT mergeVarSets(const std::vector<ReductionVarListT>& VarLists) {
  ReductionVarListT Result;
  unsigned I = trait::DIReduction::RK_First;
  unsigned EI = trait::DIReduction::RK_NumberOf;
  for (; I < EI; ++I) {
    SortedVarListT tmp;
    for (auto List : VarLists) {
      for (auto VarName : List[I]) {
        tmp.insert(VarName);
      }
    }
    Result[I] = tmp;
  }
  return Result;
}

ASTRegionTraitInfo mergeVarInfos(const std::vector<ASTRegionTraitInfo>& VarInfos) {
  std::vector<SortedVarListT> Privates, FirstPrivate, LastPrivate,
    ReadOccurreds, WriteOccurreds;
  std::vector<ReductionVarListT> Reductions;

  for (const auto& VarInfo : VarInfos) {
    Privates.push_back(VarInfo.get<trait::Private>());
    FirstPrivate.push_back(VarInfo.get<trait::FirstPrivate>());
    LastPrivate.push_back(VarInfo.get<trait::LastPrivate>());
    ReadOccurreds.push_back(VarInfo.get<trait::ReadOccurred>());
    WriteOccurreds.push_back(VarInfo.get<trait::WriteOccurred>());
    Reductions.push_back(VarInfo.get<trait::Reduction>());
  }

  return { mergeVarSets(Privates), mergeVarSets(FirstPrivate),
            mergeVarSets(LastPrivate), mergeVarSets(ReadOccurreds),
            mergeVarSets(WriteOccurreds), mergeVarSets(Reductions) };
}

class Visitor : public RecursiveASTVisitor<Visitor> {
public:

  Visitor(Decl* AST) : mAST(AST) {}

  bool VisitCallExpr(CallExpr* CE) {
    auto* Callee = CE->getCalleeDecl();
    if (Callee && mAST == Callee) {
      mExprs.push_back(CE);
    }
    return true;
  }

  const std::vector<Expr*>& getExprs() {
    return mExprs;
  }

private:
  Decl* mAST;
  std::vector<Expr*> mExprs;
};

template <class T>
bool findMemoryOverlap(T& P, DIMemoryClientServerInfo* DIMInfo,
  DominatorTree* DT, DIMemory* DIM, TargetLibraryInfo* TLI, AliasTree* AT) {
  for (auto block : P.getBlocks()) {
    for (auto I = block->begin(); I != block->end(); ++I) {
      bool Overlap = false;
      for_each_memory(*I, *TLI,
        [&Overlap, AT, DIMInfo, DT, DIM](Instruction& I, MemoryLocation&& Loc,
          unsigned Idx, AccessInfo, AccessInfo W) {
            if (W == AccessInfo::No)
              return;
            auto EM = AT->find(Loc);
            assert(EM && "Estimate memory location must not be null!");
            auto& DL = I.getModule()->getDataLayout();
            DIMemory* DIM1 = DIMInfo->findFromClient(
              *EM->getTopLevelParent(), DL, *DT).get<Clone>();
            Overlap |= (DIM1 == DIM);
        },
        [](Instruction& I, AccessInfo, AccessInfo W) {}
        );
      if (Overlap) {
        return true;
      }
    }
  }
  return false;
}

template <class T>
bool findMemoryOverlap(T& P1, T& P2, Function& F,
  TargetLibraryInfo* TLI, AliasTree* AT, DIMemoryClientServerInfo* DIMInfo,
  DominatorTree* DT, const CanonicalLoopSet* CL, DFRegionInfo* RI) {
  auto B1 = P1.getLastBlock();
  auto B2 = P2.getBlock(0);
  bool skip = true;
  bool Overlap = false;
  for (auto& CBB = F.begin(), EBB = F.end(); CBB != EBB; CBB++) {
    // try finding basic blocks between two regions.
    if (P1.contains(&*CBB)) {
      skip = false;
      continue;
    }
    if (skip) {
      continue;
    }
    if (P2.contains(&*CBB)) {
      return Overlap;
    }
    auto CanonicalItr = CL->find_as(RI->getRegionFor(B2)->getParent());
    for (auto& I : CBB->instructionsWithoutDebug()) {
      // skip debug calls and initialize instuction in for stmt
      if (auto II = llvm::dyn_cast<IntrinsicInst>(&I))
        if (isMemoryMarkerIntrinsic(II->getIntrinsicID()) ||
          isDbgInfoIntrinsic(II->getIntrinsicID()))
          continue;
      if (CanonicalItr != CL->end() && (**CanonicalItr).isCanonical()) {
        if (isa<StoreInst>(I) &&
             (*CanonicalItr)->getInduction() == I.getOperand(1)) {
          continue;
        }
      }
      if (isa<BranchInst>(I) || isa<IndirectBrInst>(I)) {
        continue;
      }
      for_each_memory(I, *TLI,
        [&Overlap, AT, DIMInfo, DT, &P1, &P2, TLI](Instruction& I,
          MemoryLocation&& Loc, unsigned Idx, AccessInfo, AccessInfo W) {
            auto EM = AT->find(Loc);
            assert(EM && "Estimate memory location must not be null!");
            auto& DL = I.getModule()->getDataLayout();
            DIMemory* DIM = DIMInfo->findFromClient(
              *EM->getTopLevelParent(), DL, *DT).get<Clone>();
            Overlap |= findMemoryOverlap(P1, DIMInfo, DT, DIM, TLI, AT);
            if (Overlap) {
              return;
            }
            Overlap |= findMemoryOverlap(P2, DIMInfo, DT, DIM, TLI, AT);
            if (Overlap) {
              return;
            }
        },
        [](Instruction& I, AccessInfo, AccessInfo W) {}
        );
      if (Overlap) {
        break;
      }
    }
    if (Overlap)
      break;
  }
  return Overlap;
}

enum PragmaType : uint8_t {
  NoType = 0u,
  Actual = 1u,
  Region = 2u,
  Parallel = 3u,
  GetActual = 4u
};

class Pragma {
protected:
  explicit Pragma(const SourceLocation& BL,
    const ASTRegionTraitInfo& VarInfo, PragmaType PragmaType, bool IsHost) :
    mBeginLoc(BL), mVarInfo(VarInfo), mPragmaType(PragmaType), mIsHost(IsHost) {}

public:
  virtual void print(tsar::TransformationContext& TfmCtx) const = 0;
  virtual void dump(tsar::TransformationContext& TfmCtx) const = 0;

  const SourceLocation& getBeginLoc() const {
    return mBeginLoc;
  }

  PragmaType getPragmaType() const {
    return mPragmaType;
  }

  const ASTRegionTraitInfo& getVarInfo() {
    return mVarInfo;
  }

  std::string getName() const {
    switch (mPragmaType) {
    case PragmaType::NoType: return "No Type";
    case PragmaType::Parallel: return "Parallel";
    case PragmaType::Region: return "Region";
    case PragmaType::Actual: return "Actual";
    case PragmaType::GetActual: return "GetActual";
    }
  }

  bool isHost() const {
    return mIsHost;
  }

protected:
  bool mIsHost;
  const SourceLocation mBeginLoc;
  const ASTRegionTraitInfo mVarInfo;
  const PragmaType mPragmaType;
};

class PragmaParallel : public Pragma {
public:
  explicit PragmaParallel(const SourceLocation& BL, DFLoop* Loop,
    const ASTRegionTraitInfo& VarInfo, bool IsHost, const PerfectLoopInfo* PLI,
    const CanonicalLoopSet* CLI) :
    Pragma(BL, VarInfo, PragmaType::Parallel, IsHost), mLoop(Loop), mPLI(PLI),
    mCLI(CLI) {};

  virtual void print(tsar::TransformationContext& TfmCtx) const override {
    SmallString<128> ParallelFor("#pragma dvm parallel (");
    if (mIsHost)
      ParallelFor += "1";
    else
      Twine(getPerfectNestSize(*mLoop, *mPLI, *mCLI))
      .toStringRef(ParallelFor);
    ParallelFor += ")";
    auto& PrivateVarsList = mVarInfo.get<trait::Private>();
    if (!PrivateVarsList.empty()) {
      ParallelFor += " private";
      addVarList(PrivateVarsList, ParallelFor);
    }
    auto& ReductionVarsList = mVarInfo.get<trait::Reduction>();
    addVarList(ReductionVarsList, ParallelFor);
    ParallelFor += "\n";
    auto& Rewriter = TfmCtx.getRewriter();
    Rewriter.InsertTextBefore(mBeginLoc, ParallelFor);
  }

  virtual void dump(tsar::TransformationContext& TfmCtx) const override {
    dbgs() << "Parallel Loc:" <<
      mBeginLoc.printToString(TfmCtx.getContext().getSourceManager()) << "\n";
  }

private:
  DFLoop* mLoop;
  const PerfectLoopInfo* mPLI;
  const CanonicalLoopSet* mCLI;

};

class PragmaRegion : public Pragma {
public:
  explicit PragmaRegion(const SourceLocation& BL, const SourceLocation& EL,
    const ASTRegionTraitInfo& VarInfo,
    std::vector<std::pair<const ForStmt*, const Loop* const>> Loops, bool IsHost) :
    Pragma(BL, VarInfo, PragmaType::Region, IsHost),
    mEndLoc(EL), mLoops(Loops) {}

  virtual void print(tsar::TransformationContext& TfmCtx) const override {
    SmallString<128> DVMHRegion("#pragma dvm region");

    if (mIsHost) {
      DVMHRegion += " targets(HOST)";
    }
    else {
      auto& ReadVarSet = mVarInfo.get<trait::ReadOccurred>();
      if (!ReadVarSet.empty()) {
        DVMHRegion += " in";
        addVarList(ReadVarSet, DVMHRegion);
      }
      auto& WriteVarSet = mVarInfo.get<trait::WriteOccurred>();
      if (!WriteVarSet.empty()) {
        DVMHRegion += " out";
        addVarList(WriteVarSet, DVMHRegion);
      }
      auto& PrivateVarSet = mVarInfo.get<trait::Private>();
      if (!PrivateVarSet.empty()) {
        DVMHRegion += " local";
        addVarList(PrivateVarSet, DVMHRegion);
      }
    }
    DVMHRegion += "\n{\n";

    auto& Rewriter = TfmCtx.getRewriter();
    Rewriter.InsertTextBefore(mBeginLoc, DVMHRegion);
    Rewriter.InsertTextAfterToken(mEndLoc, "}");
  }

  virtual void dump(tsar::TransformationContext& TfmCtx) const override {
    dbgs() << "Region Loc:" <<
      mBeginLoc.printToString(TfmCtx.getContext().getSourceManager()) << "\n";
    dbgs() << "Loops:\n";
    for (auto Loop : mLoops) {
      dbgs() << "~~~\n";
      Loop.first->dump();
      for (auto Block : Loop.second->getBlocks()) {
        if (Block) {
          Block->dump();
        }
      }
      dbgs() << "~~~\n";
    }

  }

  const ForStmt* getASTLoop(size_t I) const {
    return mLoops[I].first;
  }

  const Loop* getIRLoop(size_t I) const {
    return mLoops[I].second;
  }

  const std::vector<std::pair<const ForStmt*, const Loop* const>>& getLoops() const {
    return mLoops;
  }

  size_t getSize() const {
    return mLoops.size();
  }

  const SourceLocation& getEndLoc() const {
    return mEndLoc;
  }

private:
  const SourceLocation mEndLoc;
  const std::vector<std::pair<const ForStmt*, const Loop* const>> mLoops;
};

class PragmaActual : public Pragma {
public:
  explicit PragmaActual(const SourceLocation& BL,
    const ASTRegionTraitInfo& VarInfo, const std::vector<BasicBlock*>& Blocks,
    bool IsHost) : Pragma(BL, VarInfo, PragmaType::Actual, IsHost),
      mBlocks(Blocks) {
  };

  virtual void print(tsar::TransformationContext& TfmCtx) const override {
    SmallString<128> DVMHActual;
    if (!mIsHost) {
      auto& ReadVarSet = mVarInfo.get<trait::ReadOccurred>();
      if (!ReadVarSet.empty()) {
        DVMHActual += "#pragma dvm actual";
        addVarList(ReadVarSet, DVMHActual);
        DVMHActual += '\n';
      }
    }
    if (!DVMHActual.empty()) {
      auto& Rewriter = TfmCtx.getRewriter();
      Rewriter.InsertTextBefore(mBeginLoc, DVMHActual);
    }
  }

  virtual void dump(tsar::TransformationContext& TfmCtx) const override {
    dbgs() << "Actual Loc:" <<
      mBeginLoc.printToString(TfmCtx.getContext().getSourceManager()) << "\n";
    dbgs() << "Blocks:\n";
    for (auto& Block : mBlocks) {
      Block->dump();
    }
  }

  BasicBlock* getBlock(size_t I) {
    return mBlocks[I];
  }

  const std::vector<BasicBlock*>& getBlocks() const {
    return mBlocks;
  }

  const BasicBlock* getLastBlock() const {
    return mBlocks[mBlocks.size() - 1];
  }

  bool contains(const BasicBlock* Block) {
    return std::find(mBlocks.begin(), mBlocks.end(), Block) != mBlocks.end();
  }

private:
  const std::vector<BasicBlock*> mBlocks;
};

// TODO : maybe union with PragmaActual
class PragmaGetActual : public Pragma {
public:
  explicit PragmaGetActual(const SourceLocation& BL,
    const ASTRegionTraitInfo& VarInfo, const std::vector<BasicBlock*>& Blocks,
    bool IsHost) : Pragma(BL, VarInfo, PragmaType::GetActual, IsHost),
    mBlocks(Blocks) {};

  virtual void print(tsar::TransformationContext& TfmCtx) const override {
    SmallString<128> DVMHGetActual;
    if (!mIsHost) {
      auto& WriteVarSet = mVarInfo.get<trait::WriteOccurred>();
      if (!WriteVarSet.empty()) {
        DVMHGetActual += "#pragma dvm get_actual";
        addVarList(WriteVarSet, DVMHGetActual);
        DVMHGetActual += '\n';
      }
    }
    if (!DVMHGetActual.empty()) {
      auto& Rewriter = TfmCtx.getRewriter();
      Rewriter.InsertTextAfterToken(mBeginLoc, "\n");
      Rewriter.InsertTextAfterToken(mBeginLoc, DVMHGetActual);
    }
  }

  virtual void dump(tsar::TransformationContext& TfmCtx) const override {
    dbgs() << "GetActual Loc:" <<
      mBeginLoc.printToString(TfmCtx.getContext().getSourceManager()) << "\n";
    dbgs() << "Blocks:\n";
    for (auto& Block : mBlocks) {
      Block->dump();
    }
  }

  BasicBlock* getBlock(size_t I) {
    return mBlocks[I];
  }

  const std::vector<BasicBlock*>& getBlocks() const {
    return mBlocks;
  }

  const BasicBlock* getLastBlock() const {
    return mBlocks[mBlocks.size() - 1];
  }

  bool contains(const BasicBlock* Block) {
    return std::find(mBlocks.begin(), mBlocks.end(), Block) != mBlocks.end();
  }

private:
  const std::vector<BasicBlock*> mBlocks;
};

class PragmasInfo {
public:
  void insert(const Function* F, const Stmt* Parent, Pragma* Pragma) {
    mPragmas[F][Parent].push_back(Pragma);
    sort(mPragmas[F][Parent]);
  }

  void dump(std::string msg, const Function* F, const Loop* L,
    tsar::TransformationContext& TfmCtx) {
    dbgs() << "--------------\n";
    dbgs() << msg << "\n";
    dbgs() << "Dump for function: " << F->getName() << "\n";
    if (L != nullptr) {
      dbgs() << "Dump for loop: " << L->getName() << "\n";
    }
    for (auto& FItem : mPragmas) {
      for (auto& item : FItem.second) {
        dbgs() << "===========\n";
        dbgs() << "Parent:\n";
        //item.first->dump();
        for (const Pragma* Pragma : item.second) {
          Pragma->dump(TfmCtx);
        }
        dbgs() << "===========\n";
      }
  }
    dbgs() << "--------------\n";
  }

  virtual void print(tsar::TransformationContext& TfmCtx) const {
    for (const auto& Funcs : mPragmas) {
      for (const auto& item : Funcs.second) {
        for (const Pragma* Pragma : item.second) {
          if (Pragma->getPragmaType() == PragmaType::Parallel) {
            Pragma->print(TfmCtx);
          }
        }
        for (const Pragma* Pragma : item.second) {
          if (Pragma->getPragmaType() == PragmaType::Region) {
            Pragma->print(TfmCtx);
          }
        }
        for (const Pragma* Pragma : item.second) {
          if (Pragma->getPragmaType() == PragmaType::Actual ||
            Pragma->getPragmaType() == PragmaType::GetActual) {
            Pragma->print(TfmCtx);
          }
        }
      }
    }
  }

  /// Try union actual regions with parallel regions.
  /// From:
  ///   ....
  ///   actual1(A)
  ///   region1
  ///   get_actual(A)
  ///   actual2(B)
  ///   region2
  ///   get_actual2(B)
  ///   ....
  /// To:
  ///   ...
  ///   actual(A, B)
  ///   region
  ///   get_actual(A, B)
  ///   ...
  void tryUnionRegions(const Function* F, tsar::TransformationContext& TfmCtx) {
    dump("Befor try union regions", F, nullptr, TfmCtx);
    for (auto& item : mPragmas[F]) {
      auto Parent = item.first;
      std::vector<Pragma*> Regions;
      std::copy_if(item.second.begin(), item.second.end(),
        std::back_inserter(Regions), [](const Pragma* Pragma) {
          return Pragma->getPragmaType() == PragmaType::Region;
        });

      // Finds regions to merge
      auto& AllChildren = Parent->children();
      size_t RI = 0, LI = 0;
      std::vector<std::vector<PragmaRegion*>> NewRegions;
      std::vector<PragmaRegion*> TmpRegion;
      for (auto& CI = AllChildren.begin(), CE = AllChildren.end();
        CI != CE && RI < Regions.size(); ++CI) {
        auto CurPragmaRegion = static_cast<PragmaRegion*>(Regions[RI]);
        auto CurLoop = CurPragmaRegion->getASTLoop(LI);
        size_t RegionSize = CurPragmaRegion->getSize();
        if (static_cast<const Stmt*>(CurLoop) == *CI) {
          if (LI == RegionSize - 1) {
            TmpRegion.push_back(CurPragmaRegion);
            RI++;
            LI = 0;
          } else {
            LI++;
          }
        } else {
          if (!TmpRegion.empty()) {
            NewRegions.push_back(TmpRegion);
            TmpRegion.clear();
          }
          LI = 0;
        }
      }
      if (!TmpRegion.empty()) {
        NewRegions.push_back(TmpRegion);
      }

      std::vector<Pragma*> NewPragmas;
      // Create new regions with actual and get_actual
      for (auto& NewRegion : NewRegions) {
        assert(!NewRegion.empty());
        assert(NewRegion[0] != nullptr);

        std::vector<ASTRegionTraitInfo> VarsInfo;
        std::vector<std::pair<const ForStmt*, const Loop* const>> Loops;
        std::vector<BasicBlock*> Blocks;
        bool IsHost = false;
        for (auto OldRegion : NewRegion) {
          VarsInfo.push_back(OldRegion->getVarInfo());
          for (auto Loop : OldRegion->getLoops()) {
            Loops.push_back(Loop);
            for (auto block : Loop.second->getBlocks()) {
              Blocks.push_back(block);
            }
          }
          IsHost |= OldRegion->isHost();
        }
        auto BeginLoc = NewRegion[0]->getBeginLoc();
        auto EndLoc = NewRegion[NewRegion.size() - 1]->getEndLoc();
        NewPragmas.push_back(new PragmaRegion(BeginLoc, EndLoc,
          mergeVarInfos(VarsInfo), Loops, IsHost));
        NewPragmas.push_back(new PragmaActual(BeginLoc,
          mergeVarInfos(VarsInfo), Blocks, IsHost));
        NewPragmas.push_back(new PragmaGetActual(EndLoc,
          mergeVarInfos(VarsInfo), Blocks, IsHost));
      }
      // insert old pragmas
      std::copy_if(item.second.begin(), item.second.end(),
        std::back_inserter(NewPragmas), [](const Pragma* Pragma) {
          return Pragma->getPragmaType() == PragmaType::Parallel;
        });

      // changes vectors of pragmas
      for (Pragma* Pragma : item.second) {
        if (Pragma->getPragmaType() != PragmaType::Parallel) {
          delete Pragma;
        }
      }
      item.second = NewPragmas;
      sort(item.second);
    }
    dump("After try union regions", F, nullptr, TfmCtx);
  }

  template <class T>
  void tryUnionPragmas(Function* F, DIMemoryClientServerInfo& DIMInfo,
    ClangSMParallelProvider& Provider, tsar::TransformationContext& TfmCtx) {
    // static_assert(std::is_same<PragmaActual, T>() ||
    //   std::is_same<PragmaGetActual, T>());
    dump("before tryUnionPragmas", F, nullptr, TfmCtx);
    PragmaType Type = PragmaType::NoType;
    if constexpr (std::is_same<PragmaActual, T>()) {
      Type = PragmaType::Actual;
    }
    else {
      Type = PragmaType::GetActual;
    }
    assert(DIMInfo.isValid());
    auto DT = &Provider.get<DominatorTreeWrapperPass>().getDomTree();
    auto PDT = &Provider.get<PostDominatorTreeWrapperPass>().getPostDomTree();
    auto TLI = &Provider.get<TargetLibraryInfoWrapperPass>().getTLI();
    auto AT = &Provider.get<EstimateMemoryPass>().getAliasTree();
    auto CL = &Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
    auto RI = &Provider.get<DFRegionInfoPass>().getRegionInfo();
    auto& LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();

    for (auto& item : mPragmas[F]) {
      std::vector<Pragma*> ActualPragmas;
      std::copy_if(item.second.begin(), item.second.end(),
        std::back_inserter(ActualPragmas), [Type](const Pragma* Pragma) {
          return Pragma->getPragmaType() == Type;
        });
      if (ActualPragmas.empty()) {
        continue;
      }
      std::vector<Pragma*> SavedPragmas;
      std::copy_if(item.second.begin(), item.second.end(),
        std::back_inserter(SavedPragmas), [Type](const Pragma* Pragma) {
          return Pragma->getPragmaType() != Type;
        });
      auto CP = ActualPragmas.begin(), NP = ActualPragmas.begin();
      std::vector<std::vector<Pragma*>> UnionPragmas;
      std::vector<Pragma*> TmpUnionPragma;
      TmpUnionPragma.push_back(*CP);
      for (++NP; NP != ActualPragmas.end(); ++CP, ++NP) {
        auto CPA = static_cast<T*>(*CP);
        auto NPA = static_cast<T*>(*NP);
        auto CPAB = CPA->getBlock(0);
        auto NPAB = NPA->getBlock(0);
        auto CPAL = LI.getLoopFor(CPAB);
        auto NPAL = LI.getLoopFor(NPAB);
        bool IsPostDom = PDT->dominates(NPAB, CPAB);
        bool IsDom = DT->dominates(CPAB, NPAB);
        if (!IsPostDom || !IsDom ||
          findMemoryOverlap(*CPA, *NPA, *F, TLI, AT, &DIMInfo, DT, CL, RI)) {
          if (!TmpUnionPragma.empty()) {
            UnionPragmas.push_back(TmpUnionPragma);
            TmpUnionPragma.clear();
          }
        }
        TmpUnionPragma.push_back(*NP);
      }
      if (!TmpUnionPragma.empty()) {
        UnionPragmas.push_back(TmpUnionPragma);
      }

      //insert new pragmas
      std::vector<Pragma*> NewPragmas;
      for (auto& NewPragma : UnionPragmas) {
        assert(!NewPragma.empty());
        assert(NewPragma[0] != nullptr);
        std::vector<ASTRegionTraitInfo> VarsInfo;
        std::vector<BasicBlock*> Blocks;
        bool IsHost = false;
        for (auto OldRegion : NewPragma) {
          VarsInfo.push_back(OldRegion->getVarInfo());
          for (auto Block : static_cast<T*>(OldRegion)->getBlocks()) {
            Blocks.push_back(Block);
          }
          IsHost |= static_cast<T*>(OldRegion)->isHost();
        }
        SourceLocation BeginLoc;
        if constexpr (std::is_same<PragmaActual, T>()) {
          BeginLoc = NewPragma[0]->getBeginLoc();
        }
        else {
          BeginLoc = NewPragma[NewPragma.size() - 1]->getBeginLoc();
        }
        NewPragmas.push_back(new T(BeginLoc,
          mergeVarInfos(VarsInfo), Blocks, IsHost));
      }
      std::copy(SavedPragmas.begin(), SavedPragmas.end(),
        std::back_inserter(NewPragmas));

      // changes vectors of pragmas
      for (Pragma* Pragma : item.second) {
        if (std::find(SavedPragmas.begin(), SavedPragmas.end(), Pragma) ==
            SavedPragmas.end()) {
          delete Pragma;
        }
      }
      item.second = NewPragmas;
      sort(item.second);
    }
    dump("after tryUnionPragmas", F, nullptr, TfmCtx);
  }

  template <class T>
  void tryMovePragmasFromLoop(const Function* F, Loop* L,
    tsar::TransformationContext& TfmCtx) {
    dump("before tryMovePragmasFromLoop", F, L, TfmCtx);
    PragmaType Type = PragmaType::NoType;
    if constexpr (std::is_same<PragmaActual, T>()) {
      Type = PragmaType::Actual;
    }
    else {
      Type = PragmaType::GetActual;
    }
    DenseMap<const Stmt*, std::vector<Pragma*>> NewActualPragmas;
    for (auto& item : mPragmas[F]) {
      if (isa<CompoundStmt>(*item.first) || isa<ForStmt>(*item.first)) {
        T* CandPragma = nullptr;
        for (auto& Pragma : item.second) {
          if (Pragma->getPragmaType() == Type) {
            CandPragma = static_cast<T*>(Pragma);
            if constexpr (std::is_same<PragmaActual, T>()) {
              break;
            }
          }
        }
        if (CandPragma == nullptr) {
          continue;
        }
        // check that pragma's bloks contain in parent loop
        bool Contains = false;
        for (auto& Block : CandPragma->getBlocks()) {
          if (L->contains(Block)) {
            Contains = true;
            break;
          }
        }
        
        if (!Contains) {
          continue;
        }
        dbgs() << "Applied: " << F->getName() << "\n";

        // create copy of founded pragma
        auto& ASTCtx = TfmCtx.getContext();
        auto ASTParentLoop = !isa<ForStmt>(*item.first) ? ASTCtx.getParents(*item.first)[0].get<Stmt>() : item.first;
        if (!ASTParentLoop || !isa<ForStmt>(*ASTParentLoop)) {
          continue;
        }
        auto Parent = ASTCtx.getParents(*ASTParentLoop)[0].get<Stmt>();
        assert(Parent);
        SourceLocation SL;
        if constexpr (std::is_same<PragmaActual, T>()) {
          SL = ASTParentLoop->getLocStart();
        }
        else {
          Token SemiTok;
          SL = (!getRawTokenAfter(ASTParentLoop->getLocEnd(),
            ASTCtx.getSourceManager(), ASTCtx.getLangOpts(), SemiTok)
            && SemiTok.is(tok::semi))
            ? SemiTok.getLocation() : ASTParentLoop->getLocEnd();
        }
        auto NewActual = new T(SL, CandPragma->getVarInfo(),
          L->getBlocksVector(), CandPragma->isHost());

        //TODO: Implement split vars for new pragma and old.

        NewActualPragmas[Parent].push_back(NewActual);
      }
    }

    for (auto& Item : NewActualPragmas) {
      for (auto& Pragma : Item.second) {
        mPragmas[F][Item.first].push_back(Pragma);
      }
    }
    dump("after tryMovePragmasFromLoop", F, L, TfmCtx);
  }

  template <class T>
  void tryMovePragmasFromFunction(const Function* F,
    const std::vector<std::pair<const Function*, Instruction*>>& Callees,
    tsar::TransformationContext& TfmCtx) {
    dump("before tryMovePragmasFromFunction", F, nullptr, TfmCtx);
    PragmaType Type = PragmaType::NoType;
    if constexpr (std::is_same<PragmaActual, T>()) {
      Type = PragmaType::Actual;
    }
    else {
      Type = PragmaType::GetActual;
    }

    auto FuncDecl = TfmCtx.getDeclForMangledName(F->getName());
    if (!FuncDecl)
      return;

    T* CandPragma = nullptr;
    for (auto& Pragma : mPragmas[F][FuncDecl->getBody()]) {
      if (Pragma->getPragmaType() == Type) {
        CandPragma = static_cast<T*>(Pragma);
        if constexpr (std::is_same<PragmaActual, T>()) {
          break;
        }
      }
    }
    if (CandPragma == nullptr) {
      return;
    }
    auto& ASTCtx = TfmCtx.getContext();
    // create copies of founded pragma
    for (auto& Callee : Callees) {
      //F called by CalleeInst
      auto CalleeInst = Callee.second;
      auto CalleeBlock = CalleeInst->getParent();
      // TODO: Enable after including CallExctractor
      // assert(CalleeBlock->size() == 1);
      // F called from CalleeFunction
      auto CalleeFunction = Callee.first;
      auto CalleeFunctionDecl =
        TfmCtx.getDeclForMangledName(CalleeFunction->getName());
      Visitor V(FuncDecl);
      V.TraverseDecl(CalleeFunctionDecl);
      for (const auto& Expr : V.getExprs()) {
        SourceLocation SL;
        if (Type == PragmaType::Actual) {
          SL = Expr->getLocStart();
        }
        else {
          Token SemiTok;
          SL = (!getRawTokenAfter(Expr->getLocEnd(),
            ASTCtx.getSourceManager(), ASTCtx.getLangOpts(), SemiTok)
            && SemiTok.is(tok::semi))
            ? SemiTok.getLocation() : Expr->getLocEnd();
        }
        auto ExprParent = ASTCtx.getParents(*Expr)[0].get<Stmt>();
        insert(CalleeFunction, ExprParent, new T(SL, CandPragma->getVarInfo(),
            { CalleeBlock }, CandPragma->isHost()));
      }
    }
    dump("after tryMovePragmasFromFunction", F, nullptr, TfmCtx);
  }

  ~PragmasInfo() {
    for (const auto& Funcs : mPragmas) {
      for (const auto& item : Funcs.second) {
        for (const Pragma* Pragma : item.second) {
          delete Pragma;
        }
      }
    }
  }

private:
  void sort(std::vector<Pragma*>& Pragmas) {
    std::sort(Pragmas.begin(), Pragmas.end(),
      [](const Pragma* L, const Pragma* R) {
        if (L->getBeginLoc() < R->getBeginLoc()) {
          return true;
        }
        if (L->getBeginLoc() == R->getBeginLoc() &&
          L->getPragmaType() < R->getPragmaType()) {
          return true;
        }
        return false;
      });
  }

private:
  DenseMap<const Function*,
    DenseMap<const Stmt*, std::vector<Pragma*>>> mPragmas;
};

/// This pass try to insert OpenMP directives into a source code to obtain
/// a parallel program.
class ClangDVMHSMParallelization : public ClangSMParallelization {
public:
  static char ID;
  ClangDVMHSMParallelization() : ClangSMParallelization(ID) {
    initializeClangDVMHSMParallelizationPass(*PassRegistry::getPassRegistry());
  }
private:
  bool exploitParallelism(DFLoop& IR, const clang::ForStmt& AST,
    Function* F, const ClangSMParallelProvider& Provider,
    tsar::ClangDependenceAnalyzer& ASTDepInfo,
    TransformationContext& TfmCtx) override;

  void optimizeRegions(const Function* F, tsar::TransformationContext& TfmCtx) override;

  void optimizeLevelLoop(tsar::TransformationContext& TfmCtx, Function& F,
    Loop* L, ClangSMParallelProvider& Provider) override;

  void optimizeLevelFunction(tsar::TransformationContext& TfmCtx, Function& F,
    std::vector<std::pair<const Function*, Instruction*>>& Callees,
    ClangSMParallelProvider& Provider) override;

  void finalize(tsar::TransformationContext& TfmCtx) override;

  PragmasInfo mPragmasInfo;

  DenseMap<Function*, FunctionAnalysisResult> mFuncAnalysis;
  
  std::vector<const Loop*> mParallelLoops;

  TargetLibraryInfo* mTLI;
};

} // namespace

bool ClangDVMHSMParallelization::exploitParallelism(
    DFLoop &IR, const clang::ForStmt &AST, Function* F,
    const ClangSMParallelProvider &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis,
    TransformationContext &TfmCtx) {
  mPragmasInfo.dump("before exploit parallelism", F, IR.getLoop(), TfmCtx);
  auto& ASTCtx = TfmCtx.getContext();
  auto Parent = ASTCtx.getParents(AST)[0].get<Stmt>();
  auto& ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (!ASTDepInfo.get<trait::FirstPrivate>().empty() ||
    !ASTDepInfo.get<trait::LastPrivate>().empty())
    return false;
  auto& PI = Provider.get<ParallelLoopPass>().getParallelLoopInfo();
  bool IsHost = PI[IR.getLoop()].isHostOnly() ||
    !ASTRegionAnalysis.evaluateDefUse();
  Token SemiTok;
  auto EndLoc = (!getRawTokenAfter(AST.getLocEnd(),
    ASTCtx.getSourceManager(), ASTCtx.getLangOpts(), SemiTok)
    && SemiTok.is(tok::semi))
    ? SemiTok.getLocation() : AST.getLocEnd();
  mPragmasInfo.insert(F, Parent, new PragmaParallel(AST.getLocStart(), &IR,
      ASTDepInfo, IsHost,
    &Provider.get<ClangPerfectLoopPass>().getPerfectLoopInfo(),
    &Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo()));
  mPragmasInfo.insert(F, Parent, new PragmaRegion(AST.getLocStart(), EndLoc,
    ASTDepInfo, { {&AST, IR.getLoop()} }, IsHost));
  mPragmasInfo.insert(F, Parent, new PragmaActual(AST.getLocStart(), ASTDepInfo,
    IR.getLoop()->getBlocksVector(), IsHost));
  mPragmasInfo.insert(F, Parent, new PragmaGetActual(EndLoc, ASTDepInfo,
    IR.getLoop()->getBlocksVector(), IsHost));
  mParallelLoops.push_back(IR.getLoop());
  mPragmasInfo.dump("after exploit parallelism", F, IR.getLoop(), TfmCtx);
  return true;
}

void ClangDVMHSMParallelization::optimizeRegions(const Function* F, tsar::TransformationContext& TfmCtx) {
  mPragmasInfo.tryUnionRegions(F, TfmCtx);
}

void ClangDVMHSMParallelization::optimizeLevelLoop(
  tsar::TransformationContext& TfmCtx, Function& F, Loop* L,
  ClangSMParallelProvider& Provider) {
  if (L != nullptr && std::find(mParallelLoops.begin(), mParallelLoops.end(), L) == mParallelLoops.end()) {
    return;
  }

  auto DIAT = &Provider.get<DIEstimateMemoryPass>().getAliasTree();
  auto DIMCSI = DIMemoryClientServerInfo(*DIAT, *this, F);
  mPragmasInfo.tryUnionPragmas<PragmaActual>(&F, DIMCSI, Provider, TfmCtx);
  mPragmasInfo.tryUnionPragmas<PragmaGetActual>(&F, DIMCSI, Provider, TfmCtx);
  
  if (L != nullptr) {
    mPragmasInfo.tryMovePragmasFromLoop<PragmaActual>(&F, L, TfmCtx);
    mPragmasInfo.tryMovePragmasFromLoop<PragmaGetActual>(&F, L, TfmCtx);
    mPragmasInfo.tryUnionPragmas<PragmaActual>(&F, DIMCSI, Provider, TfmCtx);
    mPragmasInfo.tryUnionPragmas<PragmaGetActual>(&F, DIMCSI, Provider, TfmCtx);
  }
}

void ClangDVMHSMParallelization::optimizeLevelFunction(
  tsar::TransformationContext& TfmCtx, Function& F,
  std::vector<std::pair<const Function*, Instruction*>>& Callees,
  ClangSMParallelProvider& Provider) {
  mPragmasInfo.tryMovePragmasFromFunction<PragmaActual>(&F, Callees, TfmCtx);
  mPragmasInfo.tryMovePragmasFromFunction<PragmaGetActual>(&F, Callees, TfmCtx);
}

void ClangDVMHSMParallelization::finalize(
  tsar::TransformationContext& TfmCtx) {
  mPragmasInfo.print(TfmCtx);
}

ModulePass *llvm::createClangDVMHSMParallelization() {
  return new ClangDVMHSMParallelization;
}

char ClangDVMHSMParallelization::ID = 0;
INITIALIZE_SHARED_PARALLELIZATION(ClangDVMHSMParallelization,
  "clang-dvmh-sm-parallel", "Shared Memory DVMH-based Parallelization (Clang)")
