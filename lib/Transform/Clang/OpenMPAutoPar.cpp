//===-- OpenMPAutoPar.cpp - OpenMP Based Parallelization (Clang) -*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file implements a pass to perform OpenMP-based auto parallelization.
//
//===----------------------------------------------------------------------===//

#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/DIMemoryMatcher.h"
#include "tsar/Analysis/Clang/LoopMatcher.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/RegionDirectiveInfo.h"
#include "tsar/Analysis/Clang/VariableCollector.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DefinedMemory.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/LiveMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Analysis/Memory/ServerUtils.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/PassAAProvider.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Transform/IR/InterprocAttr.h"
#include <bcl/utility.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Pass.h>
#include <algorithm>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-openmp-parallel"

namespace llvm {
static void initializeClangOpenMPServerPass(PassRegistry &);
static void initializeClangOpenMPServerResponsePass(PassRegistry &);
}

namespace {
/// This provides access to function-level analysis results on server.
using ClangOpenMPServerProvider =
    FunctionPassAAProvider<DIEstimateMemoryPass, DIDependencyAnalysisPass>;

/// List of responses available from server (client may request corresponding
/// analysis, in case of provider all analysis related to a provider may
/// be requested separately).
using ClangOpenMPServerResponse = AnalysisResponsePass<
    GlobalsAAWrapperPass, DIMemoryTraitPoolWrapper, DIMemoryEnvironmentWrapper,
    GlobalDefinedMemoryWrapper, GlobalLiveMemoryWrapper,
    ClonedDIMemoryMatcherWrapper, ClangOpenMPServerProvider>;

/// This analysis server performs transformation-based analysis which is
/// necessary for OpenMP-based parallelization.
class ClangOpenMPServer final : public AnalysisServer {
public:
  static char ID;
  ClangOpenMPServer() : AnalysisServer(ID) {
    initializeClangOpenMPServerPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AnalysisServer::getAnalysisUsage(AU);
    ClientToServerMemory::getAnalysisUsage(AU);
    AU.addRequired<GlobalOptionsImmutableWrapper>();
  }

  void prepareToClone(Module &ClientM,
                      ValueToValueMapTy &ClientToServer) override {
    ClientToServerMemory::prepareToClone(ClientM, ClientToServer);
  }

  void initializeServer(Module &CM, Module &SM, ValueToValueMapTy &CToS,
                        legacy::PassManager &PM) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>();
    PM.add(createGlobalOptionsImmutableWrapper(&GO.getOptions()));
    PM.add(createGlobalDefinedMemoryStorage());
    PM.add(createGlobalLiveMemoryStorage());
    PM.add(createDIMemoryTraitPoolStorage());
    ClientToServerMemory::initializeServer(*this, CM, SM, CToS, PM);
  }

  void addServerPasses(Module &M, legacy::PassManager &PM) override {
    auto &GO = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
    addImmutableAliasAnalysis(PM);
    addBeforeTfmAnalysis(PM);
    addAfterSROAAnalysis(GO, M.getDataLayout(), PM);
    addAfterLoopRotateAnalysis(PM);
    PM.add(createVerifierPass());
    PM.add(new ClangOpenMPServerResponse);
  }

  void prepareToClose(legacy::PassManager &PM) override {
    ClientToServerMemory::prepareToClose(PM);
  }
};

/// This provider access to function-level analysis results on client.
using ClangOpenMPParalleizationProvider =
    FunctionPassAAProvider<AnalysisSocketImmutableWrapper, LoopInfoWrapperPass,
                           ParallelLoopPass, CanonicalLoopPass, LoopMatcherPass,
                           DFRegionInfoPass, ClangDIMemoryMatcherPass>;

// Sorted list of variables (to print their in algoristic order).
using SortedVarListT = VariableCollector::SortedVarListT;

// Lists of reduction variables.
using ReductionVarListT =
    std::array<SortedVarListT, trait::DIReduction::RK_NumberOf>;

/// This pass try to insert OpenMP directives into a source code to obtain
/// a parallel program.
class ClangOpenMPParalleization : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  ClangOpenMPParalleization() : ModulePass(ID) {
    initializeClangOpenMPParalleizationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override {
    mSkippedFuncs.clear();
    mRegions.clear();
    mTfmCtx = nullptr;
    mGlobalOpts = nullptr;
    mMemoryMatcher = nullptr;
    mGlobalsAA = nullptr;
    mSocket = nullptr;
  }

private:
  /// Initialize provider before on the fly passes will be run on client.
  void initializeProviderOnClient(Module &M);

  /// Initialize provider before on the fly passes will be run on server.
  void initializeProviderOnServer();

  /// Check whether it is possible to parallelize a specified loop, analyze
  /// inner loops on failure.
  bool findParallelLoops(Loop &L, Function &F,
                         ClangOpenMPParalleizationProvider &Provider);

  /// Parallelize outermost parallel loops in the range.
  template <class ItrT>
  bool findParallelLoops(ItrT I, ItrT EI, Function &F,
                         ClangOpenMPParalleizationProvider &Provider) {
    bool Parallelized = false;
    for (; I != EI; ++I)
      Parallelized |= findParallelLoops(**I, F, Provider);
    return Parallelized;
  }

  TransformationContext *mTfmCtx = nullptr;
  const GlobalOptions *mGlobalOpts = nullptr;
  MemoryMatchInfo *mMemoryMatcher = nullptr;
  GlobalsAAResult * mGlobalsAA = nullptr;
  AnalysisSocket *mSocket = nullptr;
  DenseSet<Function *> mSkippedFuncs;
  SmallVector<const OptimizationRegion *, 4> mRegions;
};

/// This specifies additional passes which must be run on client.
class ClangOpenMPParallelizationInfo final : public PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const override {
    addImmutableAliasAnalysis(Passes);
    addInitialTransformations(Passes);
    Passes.add(createAnalysisSocketImmutableStorage());
    Passes.add(createDIMemoryTraitPoolStorage());
    Passes.add(createDIMemoryEnvironmentStorage());
    Passes.add(createDIEstimateMemoryPass());
    Passes.add(new ClangOpenMPServer);
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createMemoryMatcherPass());
  }
  void addAfterPass(legacy::PassManager &Passes) const override {
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisCloseConnectionPass());
  }
};

struct ClausePrinter {
  /// Add clause for a `Trait` with variable names from a specified list to
  /// the end of `ParallelFor` pragma.
  template <class Trait> void operator()(const SortedVarListT &VarInfoList) {
    if (VarInfoList.empty())
      return;
    std::string Clause = Trait::tag::toString();
    Clause.erase(
        std::remove_if(Clause.begin(), Clause.end(), bcl::isWhitespace),
        Clause.end());
    ParallelFor += Clause;
    ParallelFor += '(';
    auto I = VarInfoList.begin(), EI = VarInfoList.end();
    ParallelFor += *I;
    for (++I; I != EI; ++I)
      ParallelFor += ", " + *I;
    ParallelFor += ')';
  }

  /// Add clauses for all reduction variables from a specified list to
  /// the end of `ParallelFor` pragma.
  template <class Trait> void operator()(const ReductionVarListT &VarInfoList) {
    unsigned I = trait::DIReduction::RK_First;
    unsigned EI = trait::DIReduction::RK_NumberOf;
    for (; I < EI; ++I) {
      if (VarInfoList[I].empty())
        continue;
      ParallelFor += "reduction";
      ParallelFor += '(';
      switch (static_cast<trait::DIReduction::ReductionKind>(I)) {
      case trait::DIReduction::RK_Add: ParallelFor += "+:"; break;
      case trait::DIReduction::RK_Mult: ParallelFor += "*:"; break;
      case trait::DIReduction::RK_Or: ParallelFor += "|:"; break;
      case trait::DIReduction::RK_And: ParallelFor += "&:"; break;
      case trait::DIReduction::RK_Xor: ParallelFor + "^: "; break;
      case trait::DIReduction::RK_Max: ParallelFor += "max:"; break;
      case trait::DIReduction::RK_Min: ParallelFor += "min:"; break;
      default: llvm_unreachable("Unknown reduction kind!"); break;
      }
      auto VarItr = VarInfoList[I].begin(), VarItrE = VarInfoList[I].end();
      ParallelFor += *VarItr;
      for (++VarItr; VarItr != VarItrE; ++VarItr)
        ParallelFor += ", " + *VarItr;
      ParallelFor += ')';
    }
  }

  SmallString<128> &ParallelFor;
};
} // namespace

bool ClangOpenMPParalleization::findParallelLoops(
    Loop &L, Function &F, ClangOpenMPParalleizationProvider &Provider) {
  if (!mRegions.empty() &&
      std::none_of(mRegions.begin(), mRegions.end(),
                   [&L](const OptimizationRegion *R) { return R->contain(L); }))
    return findParallelLoops(L.begin(), L.end(), F, Provider);
  auto &PL = Provider.get<ParallelLoopPass>().getParallelLoopInfo();
  auto &CL = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto &RI = Provider.get<DFRegionInfoPass>().getRegionInfo();
  auto &LM = Provider.get<LoopMatcherPass>().getMatcher();
  auto &SrcMgr = mTfmCtx->getRewriter().getSourceMgr();
  auto &Diags = SrcMgr.getDiagnostics();
  if (!PL.count(&L))
    return findParallelLoops(L.begin(), L.end(), F, Provider);
  auto LMatchItr = LM.find<IR>(&L);
  if (LMatchItr != LM.end())
    toDiag(Diags, LMatchItr->get<AST>()->getLocStart(),
           clang::diag::remark_parallel_loop);
  auto CanonicalItr = CL.find_as(RI.getRegionFor(&L));
  if (CanonicalItr == CL.end() || !(**CanonicalItr).isCanonical()) {
    toDiag(Diags, LMatchItr->get<AST>()->getLocStart(),
           clang::diag::warn_parallel_not_canonical);
    return findParallelLoops(L.begin(), L.end(), F, Provider);
  }
  auto *ForStmt = (**CanonicalItr).getASTLoop();
  auto RF =
      mSocket->getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(F);
  assert(RF && "Dependence analysis must be available for a parallel loop!");
  auto &DIAT = RF->value<DIEstimateMemoryPass *>()->getAliasTree();
  SpanningTreeRelation<DIAliasTree *> STR(&DIAT);
  auto &DIDepInfo = RF->value<DIDependencyAnalysisPass *>()->getDependencies();
  auto RM = mSocket->getAnalysis<AnalysisClientServerMatcherWrapper,
                                 ClonedDIMemoryMatcherWrapper>();
  assert(RM && "Client to server IR-matcher must be available!");
  auto &ClientToServer = **RM->value<AnalysisClientServerMatcherWrapper *>();
  assert(L.getLoopID() && "ID must be available for a parallel loop!");
  auto ClientLoopID = cast<MDNode>(*ClientToServer.getMappedMD(L.getLoopID()));
  auto DIDepSet = DIDepInfo[ClientLoopID];
  bcl::tagged_tuple<bcl::tagged<SortedVarListT, trait::Private>,
                    bcl::tagged<SortedVarListT, trait::FirstPrivate>,
                    bcl::tagged<SortedVarListT, trait::LastPrivate>,
                    bcl::tagged<ReductionVarListT, trait::Reduction>>
      Clauses;
  DenseSet<const DIAliasNode *> Coverage, RedundantCoverage;
  DenseSet<const DIAliasNode *> *RedundantCoverageRef = &Coverage;
  accessCoverage<bcl::SimpleInserter>(DIDepSet, DIAT, Coverage,
                                      mGlobalOpts->IgnoreRedundantMemory);
  if (mGlobalOpts->IgnoreRedundantMemory) {
    accessCoverage<bcl::SimpleInserter>(DIDepSet, DIAT,
      RedundantCoverage, false);
    RedundantCoverageRef = &RedundantCoverage;
  }
  VariableCollector ASTVars;
  ASTVars.TraverseStmt(
      const_cast<clang::ForStmt *>((*CanonicalItr)->getASTLoop()));
  auto &DIMemoryMatcher = **RM->value<ClonedDIMemoryMatcherWrapper *>();
  auto &ASTToClient = Provider.get<ClangDIMemoryMatcherPass>().getMatcher();
  DenseSet<DIAliasNode *> DirectSideEffect;
  for (auto &TS : DIDepSet) {
    if (TS.is<trait::DirectAccess>())
      for (auto &T : TS) {
        if (!T->is<trait::DirectAccess>())
          continue;
        if (auto *DIUM = dyn_cast<DIUnknownMemory>(T->getMemory()))
          if (DIUM->isExec())
            DirectSideEffect.insert(
                const_cast<DIAliasMemoryNode *>(DIUM->getAliasNode()));
      }
    MemoryDescriptor Dptr = TS;
    // This should be set to true if current node is redundant and could be
    // ignored.
    bool IgnoreRedundant = false;
    if (!Coverage.count(TS.getNode())) {
      if (mGlobalOpts->IgnoreRedundantMemory && TS.size() == 1 &&
        RedundantCoverageRef->count(TS.getNode())) {
        IgnoreRedundant = true;
        // If -fignore-redundant-memory option is set then traits for redundant
        // alias node is set to 'no access'. However, we want to consider traits
        // of redundant memory if these traits do not prevent parallelization.
        // So, if redundant node contains only single redundant memory location
        // we use traits of this location as traits of the whole node.
        Dptr = **TS.begin();
      } else {
        continue;
      }
    }
    if (Dptr.is_any<trait::Shared, trait::Readonly>()) {
      /// Remember memory locations for variables for further analysis.
      for (auto &T : TS)
        auto S = ASTVars.findDecl(*T->getMemory(), ASTToClient, DIMemoryMatcher);
    } else if (Dptr.is<trait::Induction>()) {
      if (TS.size() > 1) {
        if (!IgnoreRedundant) {
          toDiag(Diags, ForStmt->getLocStart(),
                 clang::diag::warn_parallel_loop);
          toDiag(Diags, ForStmt->getLocStart(),
                 clang::diag::note_parallel_multiple_induction);
          return findParallelLoops(L.begin(), L.end(), F, Provider);
        }
      } else {
        auto Search = ASTVars.findDecl(*(*TS.begin())->getMemory(), ASTToClient,
                                       DIMemoryMatcher);
        if (Search.second != VariableCollector::CoincideLocal ||
            Search.first != ASTVars.Induction) {
          if (!IgnoreRedundant) {
            toDiag(Diags, ForStmt->getLocStart(),
                   clang::diag::warn_parallel_loop);
            if (ASTVars.Induction && Search.first &&
                ASTVars.Induction != Search.first) {
              toDiag(Diags, ASTVars.Induction->getLocation(),
                     clang::diag::note_parallel_multiple_induction);
              toDiag(Diags, Search.first->getLocation(),
                     clang::diag::note_parallel_multiple_induction);
            } else {
              toDiag(Diags, ForStmt->getLocStart(),
                     clang::diag::note_parallel_multiple_induction);
            }
            return findParallelLoops(L.begin(), L.end(), F, Provider);
          }
        }
      }
    } else if (Dptr.is<trait::Private>()) {
      clang::VarDecl *Status = nullptr;
      if (!ASTVars.localize(TS, ASTToClient, DIMemoryMatcher,
                            Clauses.get<trait::Private>(), &Status) &&
          !IgnoreRedundant) {
        toDiag(Diags, ForStmt->getLocStart(), clang::diag::warn_parallel_loop);
        toDiag(Diags, Status ? Status->getLocation() : ForStmt->getLocStart(),
          clang::diag::note_parallel_localize_private_unable);
        return findParallelLoops(L.begin(), L.end(), F, Provider);
      }
    } else if (Dptr.is<trait::Reduction>()) {
      auto I = TS.begin(), EI = TS.end();
      auto *Red = (**I).get<trait::Reduction>();
      if (!Red || Red->getKind() == trait::DIReduction::RK_NoReduction) {
        if (!IgnoreRedundant) {
          toDiag(Diags, ForStmt->getLocStart(),
                 clang::diag::warn_parallel_loop);
          auto Search = ASTVars.findDecl(*(*I)->getMemory(), ASTToClient,
                                         DIMemoryMatcher);
          toDiag(Diags,
                 Search.first ? Search.first->getLocation()
                              : ForStmt->getLocStart(),
                 clang::diag::note_parallel_reduction_unknown);
          return findParallelLoops(L.begin(), L.end(), F, Provider);
        }
      } else {
        auto CurrentKind = Red->getKind();
        auto &ReductionList = Clauses.get<trait::Reduction>()[CurrentKind];
        clang::VarDecl *Status = nullptr;
        if (!ASTVars.localize(**I, *TS.getNode(), ASTToClient, DIMemoryMatcher,
                              ReductionList, &Status) &&
            !IgnoreRedundant) {
          toDiag(Diags, ForStmt->getLocStart(),
                 clang::diag::warn_parallel_loop);
          toDiag(Diags, Status ? Status->getLocation() : ForStmt->getLocStart(),
                 clang::diag::note_parallel_localize_reduction_unable);
          return findParallelLoops(L.begin(), L.end(), F, Provider);
        }
        for (++I; I != EI; ++I) {
          auto *Red = (**I).get<trait::Reduction>();
          if (!Red || Red->getKind() != CurrentKind) {
            if (!IgnoreRedundant) {
              toDiag(Diags, ForStmt->getLocStart(),
                clang::diag::warn_parallel_loop);
              auto Search = ASTVars.findDecl(*(*I)->getMemory(), ASTToClient,
                DIMemoryMatcher);
              toDiag(Diags,
                Search.first ? Search.first->getLocation()
                : ForStmt->getLocStart(),
                clang::diag::note_parallel_reduction_unknown);
              return findParallelLoops(L.begin(), L.end(), F, Provider);
            }
          } else {
            clang::VarDecl *Status = nullptr;
            if (!ASTVars.localize(**I, *TS.getNode(), ASTToClient,
                                  DIMemoryMatcher, ReductionList) &&
                !IgnoreRedundant) {
              toDiag(Diags, ForStmt->getLocStart(),
                     clang::diag::warn_parallel_loop);
              toDiag(Diags,
                     Status ? Status->getLocation() : ForStmt->getLocStart(),
                     clang::diag::note_parallel_localize_reduction_unable);
              return findParallelLoops(L.begin(), L.end(), F, Provider);
            }
          }
        }
      }
    } else {
      if (Dptr.is<trait::SecondToLastPrivate>()) {
        clang::VarDecl *Status = nullptr;
        if (!ASTVars.localize(TS, ASTToClient, DIMemoryMatcher,
                              Clauses.get<trait::LastPrivate>()) &&
            !IgnoreRedundant) {
          toDiag(Diags, ForStmt->getLocStart(),
                 clang::diag::warn_parallel_loop);
          toDiag(Diags, Status ? Status->getLocation() : ForStmt->getLocStart(),
                 clang::diag::note_parallel_localize_private_unable);
          return findParallelLoops(L.begin(), L.end(), F, Provider);
        }
      }
      if (Dptr.is<trait::FirstPrivate>()) {
        clang::VarDecl *Status = nullptr;
        if (!ASTVars.localize(TS, ASTToClient, DIMemoryMatcher,
                              Clauses.get<trait::FirstPrivate>()) &&
            !IgnoreRedundant) {
          toDiag(Diags, ForStmt->getLocStart(),
                 clang::diag::warn_parallel_loop);
          toDiag(Diags, Status ? Status->getLocation() : ForStmt->getLocStart(),
                 clang::diag::note_parallel_localize_private_unable);
          return findParallelLoops(L.begin(), L.end(), F, Provider);
        }
      }
    }
  }
  // Check that localization of global variables (due to private or reduction
  // clauses) does not break relation with original global variables used
  // in calls.
  for (auto &NodeWithGlobal : ASTVars.GlobalRefs)
    for (auto *NodeWithSideEffect : DirectSideEffect)
      if (!STR.isUnreachable(NodeWithGlobal.first, NodeWithSideEffect)) {
        toDiag(Diags, ForStmt->getLocStart(), clang::diag::warn_parallel_loop);
        toDiag(Diags,
               NodeWithGlobal.second ? NodeWithGlobal.second->getLocation()
                                     : ForStmt->getLocStart(),
               clang::diag::note_parallel_localize_global_unable);
        return findParallelLoops(L.begin(), L.end(), F, Provider);
      }
  // Check that traits for all variables referenced in the loop are properly
  // specified.
  for (auto &VarRef : ASTVars.CanonicalRefs)
    if (llvm::count(VarRef.second, nullptr)) {
      toDiag(Diags, ForStmt->getLocStart(), clang::diag::warn_parallel_loop);
      toDiag(Diags, VarRef.first->getLocation(),
             clang::diag::note_parallel_variable_not_analyzed)
          << VarRef.first->getName();
      return findParallelLoops(L.begin(), L.end(), F, Provider);
    }
  SmallString<128> ParallelFor("#pragma omp parallel for default(shared)");
  bcl::for_each(Clauses, ClausePrinter{ParallelFor});
  ParallelFor += '\n';
  auto &Rewriter = mTfmCtx->getRewriter();
  assert(ForStmt && "Source-level loop representation must be available!");
  Rewriter.InsertTextBefore(ForStmt->getLocStart(), ParallelFor);
  return true;
}

void ClangOpenMPParalleization::initializeProviderOnClient(Module &M) {
  ClangOpenMPParalleizationProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(mGlobalOpts);
      });
  ClangOpenMPParalleizationProvider::initialize<AnalysisSocketImmutableWrapper>(
      [this](AnalysisSocketImmutableWrapper &Wrapper) {
        Wrapper.set(*mSocket);
      });
  ClangOpenMPParalleizationProvider::initialize<TransformationEnginePass>(
      [this, &M](TransformationEnginePass &Wrapper) {
        Wrapper.setContext(M, mTfmCtx);
      });
  ClangOpenMPParalleizationProvider::initialize<MemoryMatcherImmutableWrapper>(
      [this](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*mMemoryMatcher);
      });
  ClangOpenMPParalleizationProvider::initialize<
      GlobalsAAResultImmutableWrapper>(
      [this](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(*mGlobalsAA);
      });
}

void ClangOpenMPParalleization::initializeProviderOnServer() {
  ClangOpenMPServerProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(mGlobalOpts);
      });
  auto R = mSocket->getAnalysis<GlobalsAAWrapperPass,
      DIMemoryEnvironmentWrapper, DIMemoryTraitPoolWrapper,
      GlobalDefinedMemoryWrapper, GlobalLiveMemoryWrapper>();
  assert(R && "Immutable passes must be available on server!");
  auto *DIMEnvServer = R->value<DIMemoryEnvironmentWrapper *>();
  ClangOpenMPServerProvider::initialize<DIMemoryEnvironmentWrapper>(
      [DIMEnvServer](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(**DIMEnvServer);
      });
  auto *DIMTraitPoolServer = R->value<DIMemoryTraitPoolWrapper *>();
  ClangOpenMPServerProvider::initialize<DIMemoryTraitPoolWrapper>(
      [DIMTraitPoolServer](DIMemoryTraitPoolWrapper &Wrapper) {
        Wrapper.set(**DIMTraitPoolServer);
      });
  auto &GlobalsAAServer = R->value<GlobalsAAWrapperPass *>()->getResult();
  ClangOpenMPServerProvider::initialize<GlobalsAAResultImmutableWrapper>(
      [&GlobalsAAServer](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(GlobalsAAServer);
      });
  auto *GlobalDefUseServer = R->value<GlobalDefinedMemoryWrapper *>();
  ClangOpenMPServerProvider::initialize<GlobalDefinedMemoryWrapper>(
    [GlobalDefUseServer](GlobalDefinedMemoryWrapper &Wrapper) {
      Wrapper.set(**GlobalDefUseServer);
    });
  auto *GlobalLiveMemoryServer = R->value<GlobalLiveMemoryWrapper *>();
  ClangOpenMPServerProvider::initialize<GlobalLiveMemoryWrapper>(
    [GlobalLiveMemoryServer](GlobalLiveMemoryWrapper &Wrapper) {
      Wrapper.set(**GlobalLiveMemoryServer);
    });
}

bool ClangOpenMPParalleization::runOnModule(Module &M) {
  releaseMemory();
  mTfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M.getContext().emitError("can not transform sources"
                             ": transformation context is not available");
    return false;
  }
  mSocket = &getAnalysis<AnalysisSocketImmutableWrapper>().get();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  mMemoryMatcher = &getAnalysis<MemoryMatcherImmutableWrapper>().get();
  mGlobalsAA = &getAnalysis<GlobalsAAWrapperPass>().getResult();
  initializeProviderOnClient(M);
  initializeProviderOnServer();
  auto &RegionInfo = getAnalysis<ClangRegionCollector>().getRegionInfo();
  if (mGlobalOpts->OptRegions.empty()) {
    transform(RegionInfo, std::back_inserter(mRegions),
              [](const OptimizationRegion &R) { return &R; });
  } else {
    for (auto &Name : mGlobalOpts->OptRegions)
      if (auto *R = RegionInfo.get(Name))
        mRegions.push_back(R);
      else
        toDiag(mTfmCtx->getContext().getDiagnostics(),
               clang::diag::warn_region_not_found) << Name;
  }
  auto &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  for (scc_iterator<CallGraph *> I = scc_begin(&CG); !I.isAtEnd(); ++I) {
    if (I->size() > 1)
      continue;
    auto *F = I->front()->getFunction();
    if (!F || F->isIntrinsic() || F->isDeclaration() ||
        hasFnAttr(*F, AttrKind::LibFunc) || mSkippedFuncs.count(F))
      continue;
    if (!mRegions.empty() && std::all_of(mRegions.begin(), mRegions.end(),
                                         [F](const OptimizationRegion *R) {
                                           return R->contain(*F) ==
                                                  OptimizationRegion::CS_No;
                                         }))
      continue;
    LLVM_DEBUG(dbgs() << "[OPENMP PARALLEL]: process function " << F->getName()
                      << "\n");
    auto &Provider = getAnalysis<ClangOpenMPParalleizationProvider>(*F);
    auto &LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
    findParallelLoops(LI.begin(), LI.end(), *F, Provider);
  }
  return false;
}

ModulePass *llvm::createClangOpenMPParallelization() {
  return new ClangOpenMPParalleization;
}

void ClangOpenMPParalleization::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ClangOpenMPParalleizationProvider>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<ClangRegionCollector>();
  AU.setPreservesAll();
}

INITIALIZE_PROVIDER(ClangOpenMPServerProvider, "clang-openmp-server-provider",
                    "OpenMP Based Parallelization (Clang, Server, Provider)")

template <> char ClangOpenMPServerResponse::ID = 0;
INITIALIZE_PASS(ClangOpenMPServerResponse, "clang-openmp-parallel-response",
                "OpenMP Based Parallelization (Clang, Server, Response)", true,
                false)

char ClangOpenMPServer::ID = 0;
INITIALIZE_PASS(ClangOpenMPServer, "clang-openmp-parallel-server",
                "OpenMP Based Parallelization (Clang, Server)", false, false)

INITIALIZE_PROVIDER(ClangOpenMPParalleizationProvider,
                    "clang-openmp-parallel-provider",
                    "OpenMP Based Parallelization (Clang, Provider)")

char ClangOpenMPParalleization::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(ClangOpenMPParalleization,
                               "clang-openmp-parallel",
                               "OpenMP Based Parallelization (Clang)", false,
                               false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangOpenMPParallelizationInfo)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIDependencyAnalysisPass)
INITIALIZE_PASS_DEPENDENCY(ClangOpenMPParalleizationProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(ClangOpenMPServerProvider)
INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
INITIALIZE_PASS_DEPENDENCY(ClangOpenMPServerResponse)
INITIALIZE_PASS_DEPENDENCY(ParallelLoopPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangRegionCollector)
INITIALIZE_PASS_IN_GROUP_END(ClangOpenMPParalleization, "clang-openmp-parallel",
                             "OpenMP Based Parallelization (Clang)", false,
                             false,
                             TransformationQueryManager::getPassRegistry())
