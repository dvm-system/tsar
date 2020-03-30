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
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Clang/Passes.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <string>
#include <algorithm>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-sm-parallel"

namespace {
/// This pass try to insert OpenMP directives into a source code to obtain
/// a parallel program.
class ClangDVMHSMParallelization : public ClangSMParallelization {
public:
  static char ID;
  ClangDVMHSMParallelization() : ClangSMParallelization(ID) {
    initializeClangDVMHSMParallelizationPass(*PassRegistry::getPassRegistry());
  }

  using ForVarInfoT = DenseMap<ForStmt const*,
    std::pair<ClangDependenceAnalyzer::ASTRegionTraitInfo, bool>>;

  using ParentChildInfoT = DenseMap<Stmt const*,
    std::pair<std::vector<ForStmt const*>, bool>>;

private:
  bool exploitParallelism(const Loop &IR, const clang::ForStmt &AST,
    const ClangSMParallelProvider &Provider,
    tsar::ClangDependenceAnalyzer &ASTDepInfo,
    TransformationContext &TfmCtx) override;

  void optimizeLevel(tsar::TransformationContext& TfmCtx) override;

  ForVarInfoT mForVarInfo;
  ParentChildInfoT mParentChildInfo;
};
using ForVarInfoT = ClangDVMHSMParallelization::ForVarInfoT;
using ParentChildInfoT = ClangDVMHSMParallelization::ParentChildInfoT;
using ReductionVarListT = ClangDependenceAnalyzer::ReductionVarListT;
using SortedVarListT = ClangDependenceAnalyzer::SortedVarListT;
using UnionLoopsT = std::vector<std::vector<ForStmt const*>>;

template<class T>
void unionVarSets(const std::vector<ForStmt const*>& Fors,
    ForVarInfoT& ForVarInfo, SortedVarListT& Result) {
  for (auto AST : Fors) {
    for (auto VarName : ForVarInfo[AST].first.get<T>()) {
      Result.insert(VarName);
    }
  }
}

void unionVarRedSets(const std::vector<ForStmt const*>& Fors, 
    ForVarInfoT& ForVarInfo, ReductionVarListT& Result) {
  unsigned I = trait::DIReduction::RK_First;
  unsigned EI = trait::DIReduction::RK_NumberOf;
  for (; I < EI; ++I) {
    SortedVarListT tmp;
    for (auto AST : Fors) {
      for (auto VarName : ForVarInfo[AST].first.get<trait::Reduction>()[I]) {
        tmp.insert(VarName);
      }
    }
    Result[I] = tmp;
  }
}

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
  unsigned I = trait::DIReduction::RK_First;
  unsigned EI = trait::DIReduction::RK_NumberOf;
  for (; I < EI; ++I) {
    if (VarInfoList[I].empty())
      continue;
    SmallString<7> RedKind;
    switch (static_cast<trait::DIReduction::ReductionKind>(I)) {
    case trait::DIReduction::RK_Add: RedKind += "sum"; break;
    case trait::DIReduction::RK_Mult: RedKind += "product"; break;
    case trait::DIReduction::RK_Or: RedKind += "or"; break;
    case trait::DIReduction::RK_And: RedKind += "and"; break;
    case trait::DIReduction::RK_Xor: RedKind + "xor "; break;
    case trait::DIReduction::RK_Max: RedKind += "max"; break;
    case trait::DIReduction::RK_Min: RedKind += "min"; break;
    default: llvm_unreachable("Unknown reduction kind!"); break;
    }
    ParallelFor.append({ 'r', 'e', 'd', 'u', 'c', 't', 'i', 'o', 'n' });
    ParallelFor.push_back('(');
    auto VarItr = VarInfoList[I].begin(), VarItrE = VarInfoList[I].end();
    auto k = VarItr->begin();
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

void tryUnionLoops(ParentChildInfoT& ParentChildInfo, UnionLoopsT& UnionLoops) {
  for (auto& PC : ParentChildInfo) {
    if (!PC.second.second) {
      auto& P = PC.first;
      auto& ChLps = PC.second.first;
      PC.second.second = true;
      std::sort(ChLps.begin(), ChLps.end(),
        [](const ForStmt* f1, const ForStmt* f2) {
          return f1->getBeginLoc() < f2->getBeginLoc();
        });
      auto& AllCh = P->children();
      int LI = 0;

      auto tmpUL = std::vector<ForStmt const*>();
      for (auto& CI = AllCh.begin(), CE = AllCh.end();
        CI != CE && LI < ChLps.size(); ++CI) {
        if (static_cast<Stmt const*>(ChLps[LI]) == *CI) {
          tmpUL.push_back(ChLps[LI++]);
        }
        else {
          if (!tmpUL.empty()) {
            UnionLoops.push_back(tmpUL);
            tmpUL.clear();
          }
        }
      }
      if (!tmpUL.empty()) {
        UnionLoops.push_back(tmpUL);
        tmpUL.clear();
      }
    }
  }
}
} // namespace

bool ClangDVMHSMParallelization::exploitParallelism(
    const Loop &IR, const clang::ForStmt &AST,
    const ClangSMParallelProvider &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis,
    TransformationContext &TfmCtx) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (!ASTDepInfo.get<trait::FirstPrivate>().empty() ||
      !ASTDepInfo.get<trait::LastPrivate>().empty())
    return false;
  auto& ASTCtx = TfmCtx.getContext();
  auto Parent = ASTCtx.getParents(AST)[0].get<Stmt>();
  if (mParentChildInfo.count(Parent)) {
    mParentChildInfo[Parent].first.push_back(&AST);
  } else {
    mParentChildInfo.try_emplace(Parent, std::make_pair(std::vector<ForStmt const*>({&AST}), false));
  }
  auto & PI = Provider.get<ParallelLoopPass>().getParallelLoopInfo();
  assert(!mForVarInfo.count(&AST) && "This ForStmt already in DenceMap");
  mForVarInfo.try_emplace(&AST, std::make_pair(ASTDepInfo,
    PI[&IR].isHostOnly() || !ASTRegionAnalysis.evaluateDefUse()));
   return true;
}

void ClangDVMHSMParallelization::optimizeLevel(
    tsar::TransformationContext& TfmCtx) {
  UnionLoopsT UnionLoops;
  tryUnionLoops(mParentChildInfo, UnionLoops);
  for (auto& UL : UnionLoops) {
    auto& FL = UL[0];
    auto& LL = UL[UL.size() - 1];
    bool IsHost = mForVarInfo[FL].second;
    SmallString<128> ParallelFor("#pragma dvm parallel (1)");
    SortedVarListT UnionPrivateVarSet;
    unionVarSets<trait::Private>(UL, mForVarInfo, UnionPrivateVarSet);
    if (!UnionPrivateVarSet.empty()) {
      ParallelFor += " private";
      addVarList(UnionPrivateVarSet, ParallelFor);
    }
    ReductionVarListT UnionRedVarSet;
    unionVarRedSets(UL, mForVarInfo, UnionRedVarSet);
    addVarList(UnionRedVarSet, ParallelFor);
    ParallelFor += '\n';
    SmallString<128> DVMHRegion("#pragma dvm region");
    SmallString<128> DVMHActual, DVMHGetActual;
    if (IsHost) {
      DVMHRegion += " targets(HOST)";
    }
    else {
      SortedVarListT UnionVarSet;
      unionVarSets<trait::ReadOccurred>(UL, mForVarInfo, UnionVarSet);
      if (!UnionVarSet.empty()) {
        DVMHActual += "#pragma dvm actual";
        addVarList(UnionVarSet, DVMHActual);
        DVMHRegion += " in";
        addVarList(UnionVarSet, DVMHRegion);
        DVMHActual += '\n';
        UnionVarSet.clear();
      }
      unionVarSets<trait::WriteOccurred>(UL, mForVarInfo, UnionVarSet);
      if (!UnionVarSet.empty()) {
        DVMHGetActual += "#pragma dvm get_actual";
        addVarList(UnionVarSet, DVMHGetActual);
        DVMHRegion += " out";
        addVarList(UnionVarSet, DVMHRegion);
        DVMHGetActual += '\n';
        UnionVarSet.clear();
      }
      if (!UnionPrivateVarSet.empty()) {
        DVMHRegion += " local";
        addVarList(UnionPrivateVarSet, DVMHRegion);
      }
    }
    DVMHRegion += "\n{\n";

    // Add directives to the source code.
    auto& Rewriter = TfmCtx.getRewriter();
    Rewriter.InsertTextBefore(FL->getLocStart(), ParallelFor);
    Rewriter.InsertTextBefore(FL->getLocStart(), DVMHRegion);
    if (!DVMHActual.empty())
      Rewriter.InsertTextBefore(FL->getLocStart(), DVMHActual);
    auto& ASTCtx = TfmCtx.getContext();
    Token SemiTok;
    auto InsertLoc = (!getRawTokenAfter(LL->getLocEnd(),
      ASTCtx.getSourceManager(), ASTCtx.getLangOpts(), SemiTok)
      && SemiTok.is(tok::semi))
      ? SemiTok.getLocation() : LL->getLocEnd();
    Rewriter.InsertTextAfterToken(InsertLoc, "}");
    if (!DVMHGetActual.empty()) {
      Rewriter.InsertTextAfterToken(InsertLoc, "\n");
      Rewriter.InsertTextAfterToken(InsertLoc, DVMHGetActual);
    }
  }
}

ModulePass *llvm::createClangDVMHSMParallelization() {
  return new ClangDVMHSMParallelization;
}

char ClangDVMHSMParallelization::ID = 0;
INITIALIZE_SHARED_PARALLELIZATION(ClangDVMHSMParallelization,
  "clang-dvmh-sm-parallel", "Shared Memory DVMH-based Parallelization (Clang)")
