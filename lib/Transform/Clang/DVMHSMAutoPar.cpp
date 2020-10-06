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
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Parallel/Passes.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
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
/// This pass try to insert OpenMP directives into a source code to obtain
/// a parallel program.
class ClangDVMHSMParallelization : public ClangSMParallelization {
public:
  static char ID;
  ClangDVMHSMParallelization() : ClangSMParallelization(ID) {
    initializeClangDVMHSMParallelizationPass(*PassRegistry::getPassRegistry());
  }
private:
  bool exploitParallelism(const DFLoop &IR, const clang::ForStmt &AST,
    const ClangSMParallelProvider &Provider,
    tsar::ClangDependenceAnalyzer &ASTDepInfo,
    TransformationContext &TfmCtx) override;
};

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

void addOnClause(const DFLoop &DFL, const PerfectLoopInfo &PerfectInfo,
                 const CanonicalLoopSet &CanonicalLoops,
                 const MemoryMatchInfo &MemoryMatcher,
                 SmallVectorImpl<std::pair<ObjectID, StringRef>> &ParallelNest,
                 SmallVectorImpl<char> &Out, unsigned NestSize = 0) {
  Out.append({' ', 'o', 'n', '('});
  auto *CurrDFL = &DFL;
  auto CanonicalItr = CanonicalLoops.find_as(const_cast<DFLoop *>(CurrDFL));
  assert(CanonicalItr != CanonicalLoops.end() &&
         (**CanonicalItr).isCanonical() &&
         "Parallel loop must have canonical loop form!");
  auto *Induction = (**CanonicalItr).getInduction();
  assert(Induction && "Induction variable must not be null in canonical loop!");
  auto MatchItr = MemoryMatcher.Matcher.find<IR>(Induction);
  assert(MatchItr != MemoryMatcher.Matcher.end() &&
         "AST-level variable representation must be available!");
  Out.append({'['});
  Out.append(MatchItr->get<AST>()->getName().begin(),
             MatchItr->get<AST>()->getName().end());
  Out.append({']'});
  auto LoopID = CurrDFL->getLoop()->getLoopID();
  assert(LoopID &&
         "Loop ID must be available for the outermost parallel loop!");
  ParallelNest.emplace_back(LoopID, MatchItr->get<AST>()->getName());
  for (; PerfectInfo.count(CurrDFL) && CurrDFL->getNumRegions() > 0;) {
    if (NestSize > 0 && NestSize <= ParallelNest.size())
      break;
    CurrDFL = dyn_cast<DFLoop>(*CurrDFL->region_begin());
    if (!CurrDFL)
      break;
    auto LoopID = CurrDFL->getLoop()->getLoopID();
    if (!LoopID)
      break;
    auto CanonicalItr = CanonicalLoops.find_as(const_cast<DFLoop *>(CurrDFL));
    if (CanonicalItr == CanonicalLoops.end() || !(**CanonicalItr).isCanonical())
      break;
    auto *Induction = (**CanonicalItr).getInduction();
    assert(Induction && "Induction variable must not be null in canonical loop!");
    auto MatchItr = MemoryMatcher.Matcher.find<IR>(Induction);
    assert(MatchItr != MemoryMatcher.Matcher.end() &&
          "AST-level variable representation must be available!");
    Out.append({'['});
    Out.append(MatchItr->get<AST>()->getName().begin(),
               MatchItr->get<AST>()->getName().end());
    Out.append({']'});
    ParallelNest.emplace_back(LoopID, MatchItr->get<AST>()->getName());
  }
  Out.append({')'});
}
} // namespace

bool ClangDVMHSMParallelization::exploitParallelism(
    const DFLoop &IR, const clang::ForStmt &AST,
    const ClangSMParallelProvider &Provider,
    tsar::ClangDependenceAnalyzer &ASTRegionAnalysis,
    TransformationContext &TfmCtx) {
  auto &ASTDepInfo = ASTRegionAnalysis.getDependenceInfo();
  if (!ASTDepInfo.get<trait::FirstPrivate>().empty() ||
      !ASTDepInfo.get<trait::LastPrivate>().empty())
    return false;
  SmallString<128> DVMHRegion("#pragma dvm region");
  SmallString<128> DVMHActual, DVMHGetActual;
  auto &PI = Provider.get<ParallelLoopPass>().getParallelLoopInfo();
  bool HostOnly = false;
  if (!PI[IR.getLoop()].isHostOnly() && ASTRegionAnalysis.evaluateDefUse()) {
    if (!ASTDepInfo.get<trait::ReadOccurred>().empty()) {
      DVMHActual += "#pragma dvm actual";
      addVarList(ASTDepInfo.get<trait::ReadOccurred>(), DVMHActual);
      DVMHRegion += " in";
      addVarList(ASTDepInfo.get<trait::ReadOccurred>(), DVMHRegion);
      DVMHActual += '\n';
    }
    if (!ASTDepInfo.get<trait::WriteOccurred>().empty()) {
      DVMHGetActual += "#pragma dvm get_actual";
      addVarList(ASTDepInfo.get<trait::WriteOccurred>(), DVMHGetActual);
      DVMHRegion += " out";
      addVarList(ASTDepInfo.get<trait::WriteOccurred>(), DVMHRegion);
      DVMHGetActual += '\n';
    }
    if (!ASTDepInfo.get<trait::Private>().empty()) {
      DVMHRegion += " local";
      addVarList(ASTDepInfo.get<trait::Private>(), DVMHRegion);
    }
  } else {
    DVMHRegion += " targets(HOST)";
    HostOnly = true;
  }
  auto &PerfectInfo = Provider.get<ClangPerfectLoopPass>().getPerfectLoopInfo();
  auto &CanonicalInfo = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto &MemoryMatcher = Provider.get<MemoryMatcherImmutableWrapper>().get();
  SmallString<128> ParallelFor("#pragma dvm parallel");
  SmallVector<std::pair<ObjectID, StringRef>, 4> ParallelNest;
  addOnClause(IR, PerfectInfo, CanonicalInfo, MemoryMatcher, ParallelNest,
              ParallelFor, HostOnly ? 1 : 0);
  auto *AccessInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (AccessInfo) {
    bool EmptyTie = true;
    auto arraycmp = [](const DIEstimateMemory *LHS,
                       const DIEstimateMemory *RHS) {
      return LHS->getVariable()->getName() < RHS->getVariable()->getName();
    };
    std::map<DIEstimateMemory *, SmallVector<std::string, 5>,
             decltype(arraycmp)>
        Mapping(arraycmp);
    for (auto &Access :
         AccessInfo->scope_accesses(ParallelNest.front().first)) {
      if (!isa<DIEstimateMemory>(Access.getArray()))
        continue;
      auto MappingItr =
        Mapping.emplace(std::piecewise_construct,
          std::forward_as_tuple(cast<DIEstimateMemory>(Access.getArray())),
          std::forward_as_tuple(Access.size(), "*")).first;
      auto StashSize{ ParallelFor.size() };
      for (auto *Subscript : Access) {
        if (!Subscript || MappingItr->second[Subscript->getDimension()] != "*")
          continue;
        if (auto *Affine = dyn_cast<DIAffineSubscript>(Subscript)) {
          for (unsigned I = 0, EI = Affine->getNumberOfMonoms(); I < EI; ++I) {
            if (Affine->getMonom(I).Value.isNullValue())
              continue;
            auto LoopItr = find_if(ParallelNest, [Affine, I](auto &Loop) {
              return Loop.first == Affine->getMonom(I).Column;
            });
            if (LoopItr != ParallelNest.end()) {
              MappingItr->second[Affine->getDimension()] =
                  ((Affine->getMonom(I).Value.isNegative() ? "-" : "") +
                   LoopItr->second)
                      .str();
              EmptyTie = false;
            }
          }
        }
      }
    }
    if (!EmptyTie) {
      ParallelFor += " tie(";
      for (auto &Map : Mapping) {
        if (all_of(Map.second, [](StringRef S) { return S == "*"; }))
          continue;
        ParallelFor +=
          cast<DIEstimateMemory>(Map.first)->getVariable()->getName();
        ParallelFor += "[" + join(Map.second, "][") + "]";
        ParallelFor += ",";
      }
      ParallelFor.back() = ')';
    }
  }
  if (!ASTDepInfo.get<trait::Private>().empty()) {
    ParallelFor += " private";
    addVarList(ASTDepInfo.get<trait::Private>(), ParallelFor);
  }
  addVarList(ASTDepInfo.get<trait::Reduction>(), ParallelFor);
  ParallelFor += '\n';
  DVMHRegion += "\n{\n";
  // Add directives to the source code.
  auto &Rewriter = TfmCtx.getRewriter();
  Rewriter.InsertTextBefore(AST.getBeginLoc(), ParallelFor);
  Rewriter.InsertTextBefore(AST.getBeginLoc(), DVMHRegion);
  if (!DVMHActual.empty())
    Rewriter.InsertTextBefore(AST.getBeginLoc(), DVMHActual);
  auto &ASTCtx = TfmCtx.getContext();
  Token SemiTok;
  auto InsertLoc = (!getRawTokenAfter(AST.getEndLoc(),
      ASTCtx.getSourceManager(), ASTCtx.getLangOpts(), SemiTok)
    && SemiTok.is(tok::semi))
    ? SemiTok.getLocation() : AST.getEndLoc();
  Rewriter.InsertTextAfterToken(InsertLoc, "}");
  if (!DVMHGetActual.empty()) {
    Rewriter.InsertTextAfterToken(InsertLoc, "\n");
    Rewriter.InsertTextAfterToken(InsertLoc, DVMHGetActual);
  }
  return true;
}

ModulePass *llvm::createClangDVMHSMParallelization() {
  return new ClangDVMHSMParallelization;
}

char ClangDVMHSMParallelization::ID = 0;
INITIALIZE_SHARED_PARALLELIZATION(ClangDVMHSMParallelization,
  "clang-dvmh-sm-parallel", "Shared Memory DVMH-based Parallelization (Clang)")
