//===- DataDistribtuion.cpp - Data Distribution Builder ---------*- C++ -*-===//
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
// This file implements a pass to determine rules for data distribution.
//
//===----------------------------------------------------------------------===//

#include "APCContextImpl.h"
#include "AstWrapperImpl.h"
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/Diagnostic.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/NumericUtils.h"
#include "tsar/Support/PassProvider.h"
#include "tsar/Support/SCEVUtils.h"
#include "tsar/Support/Tags.h"
#include "tsar/Support/Utils.h"
#include "tsar/Transform/Clang/DVMHDirecitves.h"
#include <apc/DirectiveProcessing/directive_creator.h>
#include <apc/DirectiveProcessing/DirectiveAnalyzer.h>
#include <apc/DirectiveProcessing/remote_access.h>
#include <apc/Distribution/CreateDistributionDirs.h>
#include <apc/GraphCall/graph_calls.h>
#include <apc/GraphCall/graph_calls_func.h>
#include <apc/GraphLoop/graph_loops.h>
#include <bcl/utility.h>
#include <bcl/ValuePtrWrapper.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/Pass.h>

#undef DEBUG_TYPE
#define DEBUG_TYPE "apc-data-distribution"

using namespace llvm;
using namespace tsar;
using namespace tsar::dvmh;

namespace {
struct DirectiveImpl : public apc::Directive {
  explicit DirectiveImpl(apc::Statement *A, std::unique_ptr<ParallelItem> PI,
                         int ShrinkedLoc, apc::Directive *Raw = nullptr,
                         bool IsOnEntry = true)
      : Anchor(A), Pragma(std::move(PI)), OnEntry(IsOnEntry), RawPragma(Raw) {
    bcl::restoreShrinkedPair(ShrinkedLoc, line, col);
  }

  apc::Statement *Anchor;
  bool OnEntry{true};
  std::unique_ptr<ParallelItem> Pragma;
  apc::Directive *RawPragma;
};

class APCDataDistributionPassInfo final : public PassGroupInfo {
  bool isNecessaryPass(llvm::AnalysisID ID) const override {
    static llvm::AnalysisID Passes[] = {
      getPassIDAndErase(createAPCLoopInfoBasePass()),
      getPassIDAndErase(createAPCArrayInfoPass()),
      getPassIDAndErase(createAPCFunctionInfoPass()),
    };
    return count(Passes, ID);
  }
};

/// Pool of descriptions of arrays accesses.
///
/// We should use pointer to apc::ArrayInfo in APC library. However, it is not
/// necessary to allocate each apc::ArrayInfo separately because it has
/// a small size.
using ArrayAccessPool =
    std::vector<bcl::ValuePtrWrapper<apc::ArrayInfo>>;

/// Map from a loop to a set of accessed arrays and descriptions of accesses.
using LoopToArrayMap =
  std::map<apc::LoopGraph *, std::map<apc::Array*, apc::ArrayInfo*>>;

class APCDataDistributionPass : public ModulePass, private bcl::Uncopyable {
  /// Map from a file name to a list of functions which are located in a file.
  using FileToFuncMap = std::map<std::string, std::vector<apc::FuncInfo *>>;

  /// Map from a formal parameter to a list of possible actual parameters.
  using FormalToActualMap = std::map<apc::Array *, std::set<apc::Array *>>;

  /// Unique key which is <Position, File, Identifier>.
  using PFIKeyT = std::tuple<int, std::string, std::string>;

  /// Map from unique array identifier to a summary of array accesses.
  ///
  /// This map does not contains arrays which are not accessed (read or write)
  /// in a program.
  ///
  /// TODO (kaniandr@gmail.com): at this moment apc::ArrayAccessInfo is not
  /// used, so it is initialized with `nullptr`.
  using ArrayAccessSummary =
    std::map<PFIKeyT, std::pair<apc::Array *, apc::ArrayAccessInfo *>>;

public:
  static char ID;

  APCDataDistributionPass() : ModulePass(ID) {
    initializeAPCDataDistributionPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module *M) const override;

private:
  bool mMultipleLaunch = false;
};

using APCDataDistributionProvider = FunctionPassProvider<
  DominatorTreeWrapperPass,
  DelinearizationPass,
  LoopInfoWrapperPass,
  ScalarEvolutionWrapperPass,
  EstimateMemoryPass,
  DIEstimateMemoryPass
>;
} // namespace

char APCDataDistributionPass::ID = 0;

INITIALIZE_PROVIDER_BEGIN(APCDataDistributionProvider, "apc-dd-provider",
  "Data Distribution Builder (APC, Provider")
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DelinearizationPass)
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
  INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PROVIDER_END(APCDataDistributionProvider, "apc-dd-provider",
  "Data Distribution Builder (APC, Provider")

INITIALIZE_PASS_IN_GROUP_BEGIN(APCDataDistributionPass, "apc-data-distribution",
  "Data Distribution Builder (APC)", true, true,
   DefaultQueryManager::PrintPassGroup::getPassRegistry())
  INITIALIZE_PASS_IN_GROUP_INFO(APCDataDistributionPassInfo);
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCDataDistributionProvider)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
  INITIALIZE_PASS_DEPENDENCY(DVMHParallelizationContext)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
  INITIALIZE_PASS_IN_GROUP_END(APCDataDistributionPass, "apc-data-distribution",
  "Data Distribution Builder (APC)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

ModulePass * llvm::createAPCDataDistributionPass() {
  return new APCDataDistributionPass;
}

void APCDataDistributionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<APCDataDistributionProvider>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<DIArrayAccessWrapper>();
  AU.addRequired<DVMHParallelizationContext>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.setPreservesAll();
}

bool APCDataDistributionPass::runOnModule(Module &M) {
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  auto *DIArrayInfo = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (!DIArrayInfo)
    return false;
  auto &DL = M.getDataLayout();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  APCDataDistributionProvider::initialize<GlobalOptionsImmutableWrapper>(
    [&GlobalOpts](GlobalOptionsImmutableWrapper &GO) {
      GO.setOptions(&GlobalOpts);
  });
  auto &MemEnv = getAnalysis<DIMemoryEnvironmentWrapper>().get();
  APCDataDistributionProvider::initialize<DIMemoryEnvironmentWrapper>(
    [&MemEnv](DIMemoryEnvironmentWrapper &Env) {
    Env.set(MemEnv);
  });
  auto &Globals{getAnalysis<GlobalsAccessWrapper>().get()};
  APCDataDistributionProvider::initialize<GlobalsAccessWrapper>(
      [&Globals](GlobalsAccessWrapper &Wrapper) { Wrapper.set(Globals); });
  auto &APCRegion = APCCtx.getDefaultRegion();
  if (!APCRegion.GetDataDir().distrRules.empty()) {
    mMultipleLaunch = true;
    return false;
  }
  // TODO (kaniandr@gmail.com): what should we do if an array is a function
  // parameter, however it is not accessed in a function. In this case,
  // this array is not processed by this pass and it will not be added in
  // a graph of arrays. So, there is no information that this array should
  // be mentioned in #pragma dvm inherit(...) directive.
  // It is not possible to delinearize this array without interprocedural
  // analysis, so it has not been stored in the list of apc::Array.
  std::map<apc::Array *, PFIKeyT> ArrayIds;
  for (auto &&[Id, A] : APCCtx.mImpl->Arrays) {
    auto &DeclInfo{*A->GetDeclInfo().begin()};
    PFIKeyT PFIKey{DeclInfo.second, DeclInfo.first, A->GetShortName()};
    ArrayIds.try_emplace(A.get(), PFIKey);
  }
  FileToFuncMap FileToFunc;
  ArrayAccessSummary ArrayRWs;
  ArrayAccessPool AccessPool;
  LoopToArrayMap Accesses;
  StringMap<bcl::tagged_pair<
      bcl::tagged<std::map<int, apc::LoopGraph *>, apc::LoopGraph>,
      bcl::tagged<std::vector<apc::ArrayRefExp *>, apc::ArrayRefExp>>>
      Loops;
  DenseMap<apc::Statement *, BasicBlock *> APCToIRLoops;
  std::map<std::string, apc::FuncInfo *> Functions;
  std::vector<apc::LoopGraph *> OuterLoops;
  auto findOrInsert = [&AccessPool, &Accesses](apc::LoopGraph *APCLoop,
                                               apc::Array *APCArray) {
    auto &APCLoopAccesses =
        Accesses
            .emplace(std::piecewise_construct, std::forward_as_tuple(APCLoop),
                     std::forward_as_tuple())
            .first->second;
    auto &APCArrayAccesses =
        APCLoopAccesses.emplace(APCArray, nullptr).first->second;
    if (!APCArrayAccesses) {
      AccessPool.emplace_back(APCArrayAccesses);
      auto &Info = AccessPool.back();
      Info->dimSize = APCArray->GetDimSize();
      Info->readOps.resize(Info->dimSize);
      Info->writeOps.resize(Info->dimSize);
      Info->unrecReadOps.resize(Info->dimSize);
    }
    return APCArrayAccesses;
  };
  auto registerUnknownRead = [&APCCtx](apc::Array *APCArray,
                                       unsigned NumberOfSubscripts,
                                       unsigned DimIdx, apc::ArrayRefExp *Expr,
                                       apc::ArrayInfo *APCArrayAccesses,
                                       apc::REMOTE_TYPE RT = REMOTE_TRUE) {
    if (RT != REMOTE_FALSE) {
      LLVM_DEBUG(dbgs() << "[APC]: register unknown read operation\n");
      APCArrayAccesses->unrecReadOps[DimIdx] = true;
    }
    auto [Itr, IsNew] = APCArrayAccesses->arrayAccessUnrec.try_emplace(Expr);
    if (IsNew) {
      Itr->second.first = 0;
      Itr->second.second.resize(NumberOfSubscripts, REMOTE_NONE);
    }
    Itr->second.second[DimIdx] |= RT;
  };
  for (auto &F : M) {
    auto *FI = APCCtx.findFunction(F);
    if (!FI)
      continue;
    LLVM_DEBUG(dbgs() << "[APC DATA DISTRIBUTION]: process function "
      << F.getName() << "\n");
    Functions.emplace(FI->funcName, FI);
    auto Itr = FileToFunc.emplace(std::piecewise_construct,
      std::forward_as_tuple(FI->fileName), std::forward_as_tuple()).first;
    Itr->second.push_back(FI);
    auto &Provider = getAnalysis<APCDataDistributionProvider>(F);
    auto &LoopsForFile = Loops.try_emplace(FI->fileName).first->second;
    for_each_loop(Provider.get<LoopInfoWrapperPass>().getLoopInfo(),
      [&APCCtx, &APCToIRLoops, &LoopsForFile](Loop *L) {
        auto ID = L->getLoopID();
        if (!ID)
          return;
        if (auto APCLoop = APCCtx.findLoop(ID)) {
          LoopsForFile.get<apc::LoopGraph>().emplace(APCLoop->lineNum, APCLoop);
          APCToIRLoops.try_emplace(APCLoop->loop, L->getHeader());
        }
      });
    for (auto *L : Provider.get<LoopInfoWrapperPass>().getLoopInfo()) {
      auto ID = L->getLoopID();
      if (!ID)
        continue;
      if (auto APCLoop = APCCtx.findLoop(ID))
        OuterLoops.push_back(APCLoop);
    }
  }
  for (auto &Access : *DIArrayInfo) {
    auto *APCArray{APCCtx.findArray(Access.getArray()->getAsMDNode())};
    if (!APCArray)
      continue;
    auto *Scope{APCCtx.findLoop(Access.getParent())};
    if (!Scope)
      continue;
    apc::ArrayRefExp *AccessExpr{nullptr};
    if (!Access.isWriteOnly()) {
      AccessExpr =
          new apc::ArrayRefExp{&APCCtx, Scope, APCArray, Access.size()};
      APCCtx.addExpression(AccessExpr);
      auto *FI{APCCtx.findFunction(
          *cast<apc::LoopStatement>(Scope->loop)->getFunction())};
      assert(FI && "Function must be registered!");
      Loops[FI->fileName].get<apc::ArrayRefExp>().push_back(AccessExpr);
    }
    LLVM_DEBUG(dbgs() << "[APC]: explore an access to "
                      << APCArray->GetShortName() << " in loop at "
                      << Scope->lineNum << "\n");
    for (unsigned DimIdx = 0, DimIdxE = Access.size(); DimIdx < DimIdxE;
         ++DimIdx) {
      auto AffineAccess = dyn_cast_or_null<DIAffineSubscript>(Access[DimIdx]);
      if (!AffineAccess) {
        if (!Access.isWriteOnly()) {
          auto *APCLoop{Scope};
          while (APCLoop) {
            assert(AccessExpr && "Expression must not be null!");
            registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                                findOrInsert(APCLoop, APCArray));
            APCLoop = APCLoop->parent;
          }
        }
        continue;
      }
      if (AffineAccess->getNumberOfMonoms() != 1) {
        if (!Access.isWriteOnly()) {
          if (AffineAccess->getNumberOfMonoms() == 0) {
            if (auto &S{AffineAccess->getSymbol()};
                S.Kind == DIAffineSubscript::Symbol::SK_Constant)
              AccessExpr->setConstantSubscript(DimIdx, S.Constant);
            auto *APCLoop{Scope};
            while (APCLoop) {
              assert(AccessExpr && "Expression must not be null!");
              registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                                  findOrInsert(APCLoop, APCArray));
              APCLoop = APCLoop->parent;
            }
          } else {
            for (unsigned I = 0, EI = AffineAccess->getNumberOfMonoms(); I < EI;
                 ++I) {
              auto &Monom{AffineAccess->getMonom(I)};
              auto *APCLoop{APCCtx.findLoop(Monom.Column)};
              assert(AccessExpr && "Expression must not be null!");
              AccessExpr->registerRecInDim(DimIdx, APCLoop);
              registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                                  findOrInsert(APCLoop, APCArray));
            }
          }
        }
        continue;
      }
      ArrayRWs.emplace(ArrayIds[APCArray], std::make_pair(APCArray, nullptr));
      auto *APCLoop = APCCtx.findLoop(AffineAccess->getMonom(0).Column);
      auto LpStmt{cast_or_null<apc::LoopStatement>(APCLoop->loop)};
      decltype(std::declval<apc::ArrayOp>().coefficients)::key_type ABPair;
      if (auto C{AffineAccess->getMonom(0).Value};
          C.Kind != DIAffineSubscript::Symbol::SK_Constant &&
              (C.Kind != DIAffineSubscript::Symbol::SK_Induction || !LpStmt ||
               C.Variable != LpStmt->getInduction().get<MD>()) ||
          !castAPInt(C.Constant, true, ABPair.first)) {
        if (!Access.isWriteOnly()) {
          assert(AccessExpr && "Expression must not be null!");
          AccessExpr->registerRecInDim(DimIdx, APCLoop);
          registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                              findOrInsert(APCLoop, APCArray));
        }
        // TODO (kaniandr@gmail.com): describe a representation issue properly.
        emitTypeOverflow(M.getContext(), *LpStmt->getFunction(), DebugLoc(),
                         "unable to represent A constant in A*I + B subscript",
                         DS_Warning);
        continue;
      }
      if (AffineAccess->getSymbol().Kind !=
                  DIAffineSubscript::Symbol::SK_Constant &&
              (AffineAccess->getSymbol().Kind !=
                   DIAffineSubscript::Symbol::SK_Induction ||
               !LpStmt ||
               AffineAccess->getSymbol().Variable !=
                   LpStmt->getInduction().get<MD>()) ||
          !castAPInt(AffineAccess->getSymbol().Constant, true, ABPair.second)) {
        if (!Access.isWriteOnly()) {
          assert(AccessExpr && "Expression must not be null!");
          AccessExpr->registerRecInDim(DimIdx, APCLoop);
          registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                              findOrInsert(APCLoop, APCArray));
        }
        // TODO (kaniandr@gmail.com): describe a representation issue properly.
        emitTypeOverflow(M.getContext(), *LpStmt->getFunction(), DebugLoc(),
                         "unable to represent B constant in A*I + B subscript",
                         DS_Warning);
        continue;
      }
      APCArray->SetMappedDim(AffineAccess->getDimension());
      auto *APCArrayAccesses{findOrInsert(APCLoop, APCArray)};
      LLVM_DEBUG(dbgs() << "[APC]: dimension " << DimIdx << " subscript "
                        << ABPair.first << " * I + " << ABPair.second
                        << " access type";
                 if (!Access.isReadOnly()) dbgs() << " write";
                 if (!Access.isWriteOnly()) dbgs() << " read"; dbgs() << "\n");
      if (!Access.isReadOnly())
        APCArrayAccesses->writeOps[DimIdx].coefficients.emplace(ABPair, 1.0);
      if (!Access.isWriteOnly()) {
        APCArrayAccesses->readOps[DimIdx].coefficients.emplace(ABPair, 1.0);
        assert(AccessExpr && "Expression must not be null!");
        AccessExpr->registerRecInDim(DimIdx, APCLoop);
        registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                            APCArrayAccesses, REMOTE_FALSE);
        auto [Itr, IsNew] =
            APCArrayAccesses->arrayAccess.try_emplace(AccessExpr);
        if (IsNew) {
          Itr->second.first = 0;
          Itr->second.second.resize(Access.size());
        }
        Itr->second.second[DimIdx].coefficients.emplace(ABPair, 1.0);
      }
    }
  }
  // Build data distribution.
  // TODO (kaniandr@gmail.com): should it be a global container?
  std::map<std::string, std::vector<apc::LoopGraph *>> FileToLoops;
  for (auto &LoopsForFile : Loops) {
    auto Itr{FileToLoops.try_emplace(LoopsForFile.first().str()).first};
    for (auto &&[Line, Loop] : LoopsForFile.second.get<apc::LoopGraph>())
      Itr->second.push_back(Loop);
  }
  std::set<apc::Array *> ArraysInAllRegions;
  std::map<std::string, std::vector<Messages>> APCMsgs;
  FormalToActualMap FormalToActual;
  auto &G{APCRegion.GetGraphToModify()};
  auto &ReducedG{APCRegion.GetReducedGraphToModify()};
  auto &AllArrays{APCRegion.GetAllArraysToModify()};
  auto &DataDirs{APCRegion.GetDataDirToModify()};
  std::vector<apc::ParallelRegion *> Regions{&APCRegion};
  std::vector<Messages> Msgs;
#ifndef LLVM_DEBUG
  try {
#endif
    checkCountOfIter(FileToLoops, FileToFunc, APCMsgs);
    createLinksBetweenFormalAndActualParams(FileToFunc, FormalToActual,
                                            ArrayRWs, APCMsgs);
    LLVM_DEBUG(
      for (auto &[Formal, ActualList] : FormalToActual) {
        dbgs() << Formal->GetName() << " -> ";
        for (auto *A : ActualList)
          dbgs() << A->GetName() << " ";
        dbgs() << "\n";
      });
    // Build a data distribution graph.
    processLoopInformationForFunction(Accesses);
    addToDistributionGraph(Accesses, FormalToActual);
    // Exclude arrays from a data distribution.
    // A stub variable which is not used at this moment.
    std::map<PFIKeyT, apc::Array *> CreatedArrays;
    excludeArraysFromDistribution(FormalToActual, ArrayRWs, FileToLoops,
                                  Regions, APCMsgs, CreatedArrays);
    for (auto LpI{Accesses.begin()}, LpEI{Accesses.end()}; LpI != LpEI;) {
      for (auto ArrayI{LpI->second.begin()}, ArrayEI{LpI->second.end()};
           ArrayI != ArrayEI;) {
        if (ArrayI->first->IsNotDistribute())
          ArrayI = LpI->second.erase(ArrayI);
        else
          ++ArrayI;
      }
      if (LpI->second.empty())
        LpI = Accesses.erase(LpI);
      else ++LpI;
    }
    // Rebuild the data distribution graph without excluded arrays.
    processLoopInformationForFunction(Accesses);
    addToDistributionGraph(Accesses, FormalToActual);
    ArraysInAllRegions.insert(AllArrays.GetArrays().begin(),
                              AllArrays.GetArrays().end());
    // Determine array distribution and alignment.
    createOptimalDistribution(G, ReducedG, AllArrays, APCRegion.GetId(), false);
    compliteArrayUsage(AllArrays, CreatedArrays, FormalToActual, ArrayIds);
    // TODO(kaniandr@gmail.com): call the next method for all parallel regions
    // at these point.
    remoteNotUsedArrays(CreatedArrays, ArraysInAllRegions, FormalToActual);
    createDistributionDirs(ReducedG, AllArrays, DataDirs, APCMsgs,
                           FormalToActual);
    createAlignDirs(ReducedG, AllArrays, DataDirs, APCRegion.GetId(),
                    FormalToActual, APCMsgs);
    // Normalize alignment, make all templates start from zero.
    shiftAlignRulesForTemplates(AllArrays.GetArrays(), APCRegion.GetId(),
                                DataDirs, FormalToActual);
    std::vector<int> FullDistrVariant;
    FullDistrVariant.reserve(DataDirs.distrRules.size());
    for (auto TplInfo : DataDirs.distrRules)
      FullDistrVariant.push_back(TplInfo.second.size() - 1);
    APCRegion.SetCurrentVariant(std::move(FullDistrVariant));
    for (auto *A : AllArrays.GetArrays()) {
      if (A->IsTemplate()) {
        auto *Tpl{ParallelCtx.makeTemplate(A->GetShortName())};
        APCCtx.addArray(Tpl, A);
        auto APCSymbol{new apc::Symbol(&APCCtx, Tpl)};
        APCCtx.addSymbol(APCSymbol);
        A->SetDeclSymbol(APCSymbol);
      }
    }
#ifdef LLVM_DEBUG
    auto dotGraphLog = [&APCRegion, &ReducedG, &AllArrays]() {
      SmallString<256> FName{"graph_reduced_with_templ_reg"};
      Twine(APCRegion.GetId()).toVector(FName);
      FName += ".dot";
      ReducedG.CreateGraphWiz(FName.c_str(),
                              std::vector<std::tuple<int, int, attrType>>(),
                              AllArrays, true);
    };
#endif
    LLVM_DEBUG(print(dbgs(), &M));
    LLVM_DEBUG(dotGraphLog());
    // Build computation distribution.
    createParallelDirectives(Accesses, Regions, FormalToActual, Msgs);
    UniteNestedDirectives(OuterLoops);
#ifndef LLVM_DEBUG
  } catch (...) {
    M.getContext().emitError(getGlobalBuffer());
    return false;
  }
#endif
  for (auto &LoopsForFile : Loops) {
    std::vector<apc::LoopGraph *> LoopList;
    for (auto &Info : LoopsForFile.second.get<apc::LoopGraph>())
      if (!Info.second->parent)
        LoopList.push_back(Info.second);
    std::vector<apc::Directive *> ParallelDirs;
    std::vector<std::pair<apc::Array*, const DistrVariant*>> CurrentVariant;
    for (std::size_t I = 0, EI = APCRegion.GetCurrentVariant().size(); I < EI;
         ++I)
      CurrentVariant.push_back(std::make_pair(
          DataDirs.distrRules[I].first,
          &DataDirs.distrRules[I].second[APCRegion.GetCurrentVariant()[I]]));
    const std::map<apc::LoopGraph *, void *> DepInfoForLoopGraph;
#ifndef LLVM_DEBUG
    try {
#endif
      selectParallelDirectiveForVariant(
          nullptr, &APCRegion, ReducedG, AllArrays, LoopList,
          LoopsForFile.second.get<apc::LoopGraph>(), Functions, CurrentVariant,
          DataDirs.alignRules, ParallelDirs, APCRegion.GetId(), FormalToActual,
          DepInfoForLoopGraph, Msgs);
      for (auto *Expr : LoopsForFile.second.get<apc::ArrayRefExp>()) {
        if (!Expr->isInLoop())
          continue;
        auto APCLoop{std::get<apc::LoopGraph *>(Expr->getScope())};
        std::vector<apc::LoopGraph *> LoopNest;
        while (APCLoop) {
          LoopNest.push_back(APCLoop);
          APCLoop = APCLoop->parent;
        }
        std::reverse(LoopNest.begin(), LoopNest.end());
        std::vector<int> NumberOfDimsWithRec(LoopNest.size(), 0);
        std::vector<int> DimWithRec(LoopNest.size(), 0);
        int NumberOfRec{0}, MaxNumberOfRecInDim{0};
        bool MultipleRecInDim{false};
        for (unsigned DimIdx = 0, DimIdxE = Expr->size(); DimIdx < DimIdxE;
             ++DimIdx) {
          for (auto *L : Expr->getRecInDim(DimIdx)) {
            auto I{find(LoopNest, L)};
            auto Depth{std::distance(LoopNest.begin(), I)};
            ++NumberOfDimsWithRec[Depth];
            MultipleRecInDim |= (NumberOfDimsWithRec[Depth] > 1);
            DimWithRec[Depth] = DimIdx;
          }
          NumberOfRec += Expr->getRecInDim(DimIdx).size();
          MaxNumberOfRecInDim = std::max<int>(MaxNumberOfRecInDim,
                                              Expr->getRecInDim(DimIdx).size());
        }
        checkArrayRefInLoopForRemoteStatus(
            MultipleRecInDim, NumberOfRec, Expr->size(), MaxNumberOfRecInDim, 0,
            Expr->getArray(), NumberOfDimsWithRec, Expr, Accesses, DimWithRec,
            LoopsForFile.second.get<apc::LoopGraph>(), FormalToActual,
            &APCRegion, LoopNest);
      }
#ifndef LLVM_DEBUG
    } catch (...) {
      M.getContext().emitError(getGlobalBuffer());
      return false;
    }
#endif
    using DirectiveList = SmallVector<DirectiveImpl *, 16>;
    DenseMap<
        Function *,
        DenseMap<apc::Statement *, std::tuple<BasicBlock *, DirectiveList>>>
        FuncToDirs;
    for (auto *Dir : ParallelDirs) {
      auto DirImpl{static_cast<DirectiveImpl *>(Dir)};
      auto *HeaderBB{APCToIRLoops[DirImpl->Anchor]};
      assert(HeaderBB && "Loop head must not be null!");
      auto FuncItr{FuncToDirs.try_emplace(HeaderBB->getParent()).first};
      auto Itr{FuncItr->second.try_emplace(DirImpl->Anchor).first};
      std::get<BasicBlock *>(Itr->second) = HeaderBB;
      std::get<DirectiveList>(Itr->second).push_back(DirImpl);
    }
    for (auto [F, DirInfo] : FuncToDirs) {
      auto &LpInfo{getAnalysis<LoopInfoWrapperPass>(*F).getLoopInfo()};
      for (auto &&[Anchor, DirList] : DirInfo) {
        auto *LpStmt{cast<apc::LoopStatement>(Anchor)};
        auto *HeaderBB{std::get<BasicBlock *>(DirList)};
        auto EntryInfo{ParallelCtx.getParallelization().try_emplace(HeaderBB)};
        assert(EntryInfo.second && "Unable to create a parallel block!");
        EntryInfo.first->get<ParallelLocation>().emplace_back();
        EntryInfo.first->get<ParallelLocation>().back().Anchor =
            LpStmt->getId();
        auto *LLVMLoop{ LpInfo.getLoopFor(HeaderBB) };
        assert(LLVMLoop && "LLVM IR representation of a loop must be known!");
        auto ExitingBB{LLVMLoop->getExitingBlock()};
        assert(ExitingBB && "Parallel loop must have a single exit!");
        ParallelLocation *ExitLoc{nullptr};
        if (ExitingBB == LLVMLoop->getHeader()) {
          ExitLoc = &EntryInfo.first->get<ParallelLocation>().back();
        } else {
          auto ExitInfo{
              ParallelCtx.getParallelization().try_emplace(ExitingBB)};
          assert(ExitInfo.second && "Unable to create a parallel block!");
          ExitInfo.first->get<ParallelLocation>().emplace_back();
          ExitLoc = &ExitInfo.first->get<ParallelLocation>().back();
          ExitLoc->Anchor = LpStmt->getId();
        }
        std::unique_ptr<PragmaParallel> DVMHParallel;
        for (auto &Dir : std::get<DirectiveList>(DirList)) {
          if (isa<PragmaRealign>(Dir->Pragma) && Dir->OnEntry) {
            EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
              std::move(Dir->Pragma));
            delete Dir;
            Dir = nullptr;
          } else if (isa<PragmaParallel>(Dir->Pragma)) {
            assert(Dir->RawPragma &&
                   "Raw representation of a parallel directive "
                   "must be available!");
#ifndef LLVM_DEBUG
            try {
#endif
              auto RemoteList{createRemoteInParallel(
                  std::pair{
                      LpStmt->getLoop(),
                      static_cast<apc::ParallelDirective *>(Dir->RawPragma)},
                  AllArrays, Accesses, ReducedG, DataDirs,
                  APCRegion.GetCurrentVariant(), Msgs, APCRegion.GetId(),
                  FormalToActual)};
              for (auto &&[Str, Expr] : RemoteList) {
                dvmh::Align Align;
                auto S{Expr->getArray()->GetDeclSymbol()};
                Align.Target = S->getVariable(F).getValue();
                Align.Relation.resize(Expr->size());
                for (unsigned I = 0, EI = Expr->size(); I < EI; ++I) {
                  if (Expr->isConstantSubscript(I))
                    Align.Relation[I] = Expr->getConstantSubscript(I);
                }
                cast<PragmaParallel>(*Dir->Pragma)
                    .getClauses()
                    .get<Remote>()
                    .insert(std::move(Align));
              }
#ifndef LLVM_DEBUG
            } catch (...) {
              M.getContext().emitError(getGlobalBuffer());
              return false;
            }
#endif
            DVMHParallel.reset(cast<PragmaParallel>(Dir->Pragma.release()));
            delete Dir;
            Dir = nullptr;
          }
        }
        std::unique_ptr<PragmaActual> DVMHActual;
        std::unique_ptr<PragmaGetActual> DVMHGetActual;
        std::unique_ptr<PragmaRegion> DVMHRegion;
        if (!LpStmt->isHostOnly()) {
          DVMHRegion = std::make_unique<PragmaRegion>();
          DVMHRegion->finalize();
          DVMHRegion->child_insert(DVMHParallel.get());
          DVMHParallel->parent_insert(DVMHRegion.get());
          if (!LpStmt->getTraits().get<trait::ReadOccurred>().empty()) {
            DVMHActual = std::make_unique<PragmaActual>(false);
            DVMHActual->getMemory().insert(
                LpStmt->getTraits().get<trait::ReadOccurred>().begin(),
                LpStmt->getTraits().get<trait::ReadOccurred>().end());
            DVMHRegion->getClauses().get<trait::ReadOccurred>().insert(
                LpStmt->getTraits().get<trait::ReadOccurred>().begin(),
                LpStmt->getTraits().get<trait::ReadOccurred>().end());
          }
          if (!LpStmt->getTraits().get<trait::WriteOccurred>().empty()) {
            DVMHGetActual = std::make_unique<PragmaGetActual>(false);
            DVMHGetActual->getMemory().insert(
                LpStmt->getTraits().get<trait::WriteOccurred>().begin(),
                LpStmt->getTraits().get<trait::WriteOccurred>().end());
            DVMHRegion->getClauses().get<trait::WriteOccurred>().insert(
                LpStmt->getTraits().get<trait::WriteOccurred>().begin(),
                LpStmt->getTraits().get<trait::WriteOccurred>().end());
          }
          DVMHRegion->getClauses().get<trait::Private>().insert(
              LpStmt->getTraits().get<trait::Private>().begin(),
              LpStmt->getTraits().get<trait::Private>().end());
        }
        if (DVMHRegion)
          ExitLoc->Exit.push_back(
              std::make_unique<ParallelMarker<PragmaRegion>>(0,
                                                             DVMHRegion.get()));
        if (DVMHActual)
          EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
              std::move(DVMHActual));
        if (DVMHRegion)
          EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
              std::move(DVMHRegion));
        EntryInfo.first->get<ParallelLocation>().back().Entry.push_back(
            std::move(DVMHParallel));
        if (DVMHGetActual)
          ExitLoc->Exit.push_back(std::move(DVMHGetActual));
        for (auto &Dir : std::get<DirectiveList>(DirList)) {
          if (Dir && isa<PragmaRealign>(Dir->Pragma) && !Dir->OnEntry) {
            ExitLoc->Exit.push_back(std::move(Dir->Pragma));
            delete Dir;
            Dir = nullptr;
          }
        }
        assert(all_of(std::get<DirectiveList>(DirList),
                      [](auto *Dir) { return Dir == nullptr; }) &&
               "Memory leak!");
      }
    }
  }
  std::set<apc::Array *> DistributedArrays;
#ifndef LLVM_DEBUG
  try {
#endif
    copy_if(AllArrays.GetArrays(),
            std::inserter(DistributedArrays, DistributedArrays.end()),
            [](auto *A) { return !A->IsNotDistribute(); });
    createShadowSpec(FileToLoops, FormalToActual, DistributedArrays);
#ifndef LLVM_DEBUG
  } catch (...) {
    M.getContext().emitError(getGlobalBuffer());
    return false;
  }
#endif
  auto &TLIP{getAnalysis<TargetLibraryInfoWrapperPass>()};
  for (auto &ClientF : M) {
    auto *FI = APCCtx.findFunction(ClientF);
    if (!FI)
      continue;
    auto &TLI{TLIP.getTLI(ClientF)};
    auto &Provider{getAnalysis<APCDataDistributionProvider>(ClientF)};
    auto &ClientDIAT{Provider.get<DIEstimateMemoryPass>().getAliasTree()};
    DIMemoryClientServerInfo ClientServerInfo(ClientDIAT, *this, ClientF);
    auto &F{cast<Function>(*ClientServerInfo.getValue(&ClientF))};
    SmallPtrSet<const DIAliasNode *, 8> DistinctMemory;
    for (auto &DIM :
         make_range(ClientServerInfo.DIAT->memory_begin(), ClientServerInfo.DIAT->memory_end()))
      if (auto *DIUM{dyn_cast<DIUnknownMemory>(&DIM)};
          DIUM && DIUM->isDistinct())
        DistinctMemory.insert(DIUM->getAliasNode());
    SpanningTreeRelation<const DIAliasTree *> STR{ClientServerInfo.DIAT};
    DenseMap<const DIAliasNode *, SmallVector<VariableT, 1>> ToDistribute;
    for (auto *A : DistributedArrays) {
      if (A->IsTemplate())
        continue;
      auto *S{A->GetDeclSymbol()};
      auto Var{S->getVariable(&ClientF)};
      if (!Var)
        continue;
      auto DIMI{ClientServerInfo.getMemory(&*Var->get<MD>())};
      auto I{ToDistribute.try_emplace(DIMI->getAliasNode()).first};
      if (!is_contained(I->second, *Var))
        I->second.push_back(std::move(*Var));
    }
    auto &LI{Provider.get<LoopInfoWrapperPass>().getLoopInfo()};
    Optional<SmallVector<dvmh::VariableT, 8>> ToAccessDistinct;
    for (auto &I : instructions(&ClientF)) {
      // Do not attach remote_access to calls. It has to be inserted in the body
      // of callee. If body is not available we cannot distribute an accessed
      // array. This check must be performed earlier.
      if (isa<CallBase>(I))
        continue;
      if (isInParallelNest(I, LI, ParallelCtx.getParallelization()).first)
        continue;
      SmallVector<dvmh::VariableT, 8> ToAccess;
      auto addToAccessIfNeed = [&STR, &ToDistribute](auto *CurrentAN,
                                                             auto &ToAccess) {
        if (auto Itr{find_if(ToDistribute,
                             [&STR, CurrentAN](auto &Data) {
                               return !STR.isUnreachable(Data.first,
                                                               CurrentAN);
                             })};
            Itr != ToDistribute.end())
          ToAccess.append(Itr->second.begin(), Itr->second.end());
      };
      bool IsDistinctAccessed{false};
      for_each_memory(
          I, TLI,
          [&DL = ClientF.getParent()->getDataLayout(), &Provider,
           &ClientServerInfo, &ToDistribute, &addToAccessIfNeed, &ToAccess,
           &IsDistinctAccessed](Instruction &I, MemoryLocation &&Loc,
                                unsigned OpIdx, AccessInfo IsRead,
                                AccessInfo IsWrite) {
            if (IsRead == AccessInfo::No)
              return;
            auto &AT{Provider.get<EstimateMemoryPass>().getAliasTree()};
            auto &DT{Provider.get<DominatorTreeWrapperPass>().getDomTree()};
            auto *EM{AT.find(Loc)};
            assert(EM && "Estimate memory must be presented in alias tree!");
            auto CToSDIM{ ClientServerInfo.findFromClient(*EM, DL, DT) };
            if (!CToSDIM.get<Origin>()) {
              IsDistinctAccessed = true;
              return;
            }
            addToAccessIfNeed(CToSDIM.get<Clone>()->getAliasNode(), ToAccess);
          },
          [&Provider, &ClientServerInfo, &addToAccessIfNeed, &ToAccess,
           &IsDistinctAccessed](Instruction &I, AccessInfo IsRead,
                                AccessInfo IsWrite) {
            if (IsRead == AccessInfo::No)
              return;
            auto &AT{Provider.get<EstimateMemoryPass>().getAliasTree()};
            auto *AN{AT.findUnknown(I)};
            if (!AN)
              return;
            auto &DT{Provider.get<DominatorTreeWrapperPass>().getDomTree()};
            auto CToSDIM{ClientServerInfo.findFromClient(
                I, DT, tsar::DIUnknownMemory::NoFlags)};
            if (!CToSDIM.get<Origin>()) {
              IsDistinctAccessed = true;
              return;
            }
            addToAccessIfNeed(CToSDIM.get<Clone>()->getAliasNode(), ToAccess);
          });
      if (IsDistinctAccessed && !ToAccessDistinct) {
        ToAccessDistinct.emplace();
        for (auto *AN : DistinctMemory)
          addToAccessIfNeed(AN, *ToAccessDistinct);
      }
      if (!ToAccess.empty() ||
          IsDistinctAccessed && !ToAccessDistinct->empty()) {
        auto RemoteRef{
            ParallelCtx.getParallelization().emplace<PragmaRemoteAccess>(
                I.getParent(), &I, true /*OnEntry*/, false /*IsRequired*/,
                false /*IsFinal*/)};
        cast<PragmaRemoteAccess>(RemoteRef)->getMemory().insert(
            ToAccess.begin(), ToAccess.end());
        if (IsDistinctAccessed)
          cast<PragmaRemoteAccess>(RemoteRef)->getMemory().insert(
              ToAccessDistinct->begin(), ToAccessDistinct->end());
      }
    }
  }
  return false;
}

void APCDataDistributionPass::print(raw_ostream &OS, const Module *M) const {
  if (mMultipleLaunch)
    OS << "warning: possible multiple launches of the pass for the same "
          "module: print results for the first successful launch\n";
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  auto &APCRegion = APCCtx.getDefaultRegion();
  auto &DataDirs = APCRegion.GetDataDir();
  dbgs() << "List of templates:\n";
  for (auto &TemplateInfo : DataDirs.distrRules) {
    dbgs() << "  " << TemplateInfo.first->GetShortName().c_str();
    for (auto S : TemplateInfo.first->GetSizes())
      dbgs() << "[" << S.first << ":" << S.second << "]";
    dbgs() << "\n";
  }
  dbgs() << "List of aligns:\n";
  for (auto &Rule : DataDirs.GenAlignsRules())
    OS << "  " << Rule << "\n";
  for (auto &AR : DataDirs.alignRules)
    AR.alignArray->GetShadowSpec();
}

apc::Directive * ParallelDirective::genDirective(File *F,
    const std::vector<std::pair<apc::Array *, const apc::DistrVariant *>>
        &Distribution,
    const std::vector<apc::AlignRule> &AlignRules, apc::LoopGraph *CurrLoop,
    apc::GraphCSR<int, double, attrType> &ReducedG,
    apc::Arrays<int> &AllArrays,
    const uint64_t RegionId,
    const std::map<apc::Array *, std::set<apc::Array *>>
        &ArrayLinksByFuncCalls) {
#ifdef LLVM_DEBUG
  auto parallelLog = [CurrLoop]() {
    dbgs() << "[APC]: generate a parallel directive for ";
    std::pair<unsigned, unsigned> StartLoc, EndLoc;
    bcl::restoreShrinkedPair(CurrLoop->lineNum, StartLoc.first,
                             StartLoc.second);
    bcl::restoreShrinkedPair(CurrLoop->lineNumAfterLoop, EndLoc.first,
                             EndLoc.second);
    dbgs() << "a loop at " << CurrLoop->fileName
           << format(":[%d:%d,%d:%d](shrink:[%d,%d])\n", StartLoc.first,
                     StartLoc.second, EndLoc.first, EndLoc.second,
                     CurrLoop->lineNum, CurrLoop->lineNumAfterLoop);
  };
#endif
  LLVM_DEBUG(parallelLog());
  cast<apc::LoopStatement>(CurrLoop->loop)->scheduleToParallelization();
  auto *DirImpl{new DirectiveImpl{CurrLoop->loop,
                                  std::make_unique<PragmaParallel>(),
                                  CurrLoop->lineNum, this}};
  auto *Parallel{cast<PragmaParallel>(DirImpl->Pragma.get())};
  const auto &ReadOps{CurrLoop->readOps};
  auto &Clauses{Parallel->getClauses()};
  auto *Func{cast<apc::LoopStatement>(CurrLoop->loop)->getFunction()};
  SmallVector<apc::LoopGraph *, 4> Nest;
  auto LoopInNest{CurrLoop};
  while (LoopInNest->perfectLoop) {
    Nest.emplace_back(LoopInNest);
    if (LoopInNest->children.empty() || LoopInNest->children.size() > 1)
      break;
    LoopInNest = LoopInNest->children.front();
  }
  for (unsigned I = 0, EI = parallel.size(); I < EI; ++I) {
    if (parallel[I] != "*") {
      auto Itr{find_if(Nest, [&Name = parallel[I]](auto &L) {
        auto LpStmt{cast<apc::LoopStatement>(L->loop)};
        return LpStmt->getInduction().template get<AST>()->getName() == Name;
      })};
      assert(Itr != Nest.end() && "Unknown parallel loop!");
      auto LpStmt{cast<apc::LoopStatement>((**Itr).loop)};
      Clauses.get<trait::Induction>().emplace_back(LpStmt->getId(),
                                                   LpStmt->getInduction());
      for (unsigned I = 0, EI = Clauses.get<trait::Reduction>().size(); I < EI;
           ++I)
        Clauses.get<trait::Reduction>()[I].insert(
            LpStmt->getTraits().get<trait::Reduction>()[I].begin(),
            LpStmt->getTraits().get<trait::Reduction>()[I].end());
      Clauses.get<trait::Private>().insert(
          LpStmt->getTraits().get<trait::Private>().begin(),
          LpStmt->getTraits().get<trait::Private>().end());
    }
  }
  for (auto &Induct : Clauses.get<trait::Induction>())
    Clauses.get<trait::Private>().erase(Induct.get<VariableT>());
  auto *MapTo{arrayRef2->IsLoopArray() ? arrayRef : arrayRef2};
  Clauses.get<dvmh::Align>() = dvmh::Align{};
  auto APCSymbol{MapTo->GetDeclSymbol()};
  assert(APCSymbol && "Unknown array symbol!");
  if (APCSymbol->isTemplate())
    Clauses.get<dvmh::Align>()->Target = APCSymbol->getTemplate();
  else {

    Clauses.get<dvmh::Align>()->Target =
      APCSymbol->getVariable(Func).getValue();

  }
  auto &OnTo{arrayRef2->IsLoopArray() ? on : on2};
  auto LpStmt{cast<apc::LoopStatement>(CurrLoop->loop)};
  auto &Start{LpStmt->getStart()};
  auto &Step{LpStmt->getStep()};
  APSInt StartTfm{APInt{64, 0, true}, false};
  APSInt StepTfm{APInt{64, 1, true}, false};
  auto BitWidth{StartTfm.getBitWidth()};
  if (Start && Step) {
    StartTfm = *Start;
    if (StartTfm.isUnsigned())
      StartTfm = APSInt{StartTfm.zext(StartTfm.getBitWidth() * 2), false};
    StepTfm = *Step;
    if (StepTfm.isUnsigned())
      StepTfm = APSInt{StepTfm.zext(StepTfm.getBitWidth() * 2), false};
    BitWidth = std::max(BitWidth, StartTfm.getBitWidth());
    BitWidth = std::max(BitWidth, StepTfm.getBitWidth());
    if (StartTfm.getBitWidth() < BitWidth)
      StartTfm = StartTfm.extend(BitWidth);
    if (StepTfm.getBitWidth() < BitWidth)
      StepTfm = StepTfm.extend(BitWidth);
  }
  for (unsigned I = 0, EI = OnTo.size(); I < EI; ++I) {
    if (OnTo[I].first == "*" ||
        OnTo[I].second.first == 0 && OnTo[I].second.second == 0) {
      Clauses.get<dvmh::Align>()->Relation.emplace_back(None);
      continue;
    }
    if (OnTo[I].second.first == 0) {
      Clauses.get<dvmh::Align>()->Relation.emplace_back(
          APSInt{APInt{64, (uint64_t)OnTo[I].second.second, true}, false});
      continue;
    }
    auto Itr{find_if(Clauses.get<trait::Induction>(), [&Name = OnTo[I].first](
                                                          auto &L) {
      return L.template get<VariableT>().template get<AST>()->getName() == Name;
    })};
    assert(Itr != Clauses.get<trait::Induction>().end() &&
           "Unknown axis in parallel directive");
    dvmh::Align::Axis Axis;
    Axis.Dimension =
        std::distance(Clauses.get<trait::Induction>().begin(), Itr);
    Axis.Offset =
        APSInt{APInt{BitWidth, (uint64_t)OnTo[I].second.second, true}, false};
    Axis.Offset = Axis.Offset - StartTfm;
    Axis.Step =
        APSInt{APInt{BitWidth, (uint64_t)OnTo[I].second.first, true}, false};
    Axis.Step = Axis.Step / StepTfm;
    Clauses.get<dvmh::Align>()->Relation.emplace_back(Axis);
  }
  std::set<apc::Array *> ArraysInAcross;
  if (!across.empty()) {
    if (acrossShifts.empty()) {
      acrossShifts.resize(across.size());
      for (unsigned I = 0, EI = across.size(); I < EI; ++I)
        acrossShifts[I].resize(across[I].second.size());
    }
    for (unsigned I = 0, EI = across.size(); I < EI; ++I) {
      std::vector<std::map<std::pair<int, int>, int>> ShiftsByAccess;
      auto Bounds{genBounds(AlignRules, across[I], acrossShifts[I], ReducedG,
                            AllArrays, CurrLoop->remoteRegularReads, ReadOps,
                            true, RegionId, Distribution, ArraysInAcross,
                            ShiftsByAccess, ArrayLinksByFuncCalls)};
      if (Bounds.empty())
        continue;
      apc::Array *A{AllArrays.GetArrayByName(across[I].first.second)};
      assert(A && "Array must be known!");
      assert(!A->IsTemplate() &&
             "Template cannot be referenced in across clause!");
      auto AcrossItr{
          Clauses.get<trait::Dependence>()
              .try_emplace(A->GetDeclSymbol()->getVariable(Func).getValue())
              .first};
      for (unsigned Dim = 0, DimE = across[I].second.size(); Dim < DimE;
           ++Dim) {
        auto Left{across[I].second[Dim].first + acrossShifts[I][Dim].first};
        auto Right{across[I].second[Dim].second + acrossShifts[I][Dim].second};
        AcrossItr->second.emplace_back(
            APSInt{APInt{64, (uint64_t)Left, false}, true},
            APSInt{APInt{64, (uint64_t)Right, false}, true});
      }
    }
  }
  if (!shadowRenew.empty()) {
    if (shadowRenewShifts.empty()) {
      shadowRenewShifts.resize(shadowRenew.size());
      for (unsigned I = 0, EI = shadowRenew.size(); I < EI; ++I)
        shadowRenewShifts[I].resize(shadowRenew[I].second.size());
    }
    for (unsigned I = 0, EI = shadowRenew.size(); I < EI; ++I) {
      std::vector<std::map<std::pair<int, int>, int>> ShiftsByAccess;
      auto Bounds{genBounds(
          AlignRules, shadowRenew[I], shadowRenewShifts[I], ReducedG, AllArrays,
          CurrLoop->remoteRegularReads, ReadOps, false, RegionId, Distribution,
          ArraysInAcross, ShiftsByAccess, ArrayLinksByFuncCalls)};
      if (Bounds.empty())
        continue;
      apc::Array *A{AllArrays.GetArrayByName(shadowRenew[I].first.second)};
      assert(A && "Array must be known!");
      assert(!A->IsTemplate() &&
             "Template cannot be referenced in shadow_renew clause!");
      auto ShadowItr{
          Clauses.get<Shadow>()
              .try_emplace(A->GetDeclSymbol()->getVariable(Func).getValue())
              .first};
      for (unsigned Dim = 0, DimE = shadowRenew[I].second.size(); Dim < DimE;
           ++Dim) {
        auto Left{shadowRenew[I].second[Dim].first +
                  shadowRenewShifts[I][Dim].first};
        auto Right{shadowRenew[I].second[Dim].second +
                   shadowRenewShifts[I][Dim].second};
        ShadowItr->second.emplace_back(
            APSInt{APInt{64, (uint64_t)Left, false}, true},
            APSInt{APInt{64, (uint64_t)Right, false}, true});
      }
    }
  }
  Parallel->finalize();
  return DirImpl;
}

void fillAcrossInfoFromDirectives(
    const LoopGraph *L,
    std::vector<std::pair<std::pair<std::string, std::string>,
                          std::vector<std::pair<int, int>>>> &AcrossInfo) {
  auto LpStmt{cast_or_null<apc::LoopStatement>(L->loop)};
  if (!LpStmt)
    return;
  for (auto &&[Var, Distance] : LpStmt->getTraits().get<trait::Dependence>()) {
    AcrossInfo.emplace_back();
    AcrossInfo.back().first.first = Var.get<AST>()->getName().str();
    auto DIEM{cast<DIEstimateMemory>(Var.get<MD>())};
    AcrossInfo.back().first.second =
        APCContext::getUniqueName(*DIEM->getVariable(), *LpStmt->getFunction());
    for (auto Range : Distance) {
      int Left{0}, Right{0};
      if (Range.first)
        Left = Range.first->getSExtValue();
      if (Range.second)
        Right = Range.second->getSExtValue();
      AcrossInfo.back().second.emplace_back(Left, Right);
    }
  }
}

void fillInfoFromDirectives(const LoopGraph *L,
                            ParallelDirective *D) {
  fillAcrossInfoFromDirectives(L, D->across);
}

std::pair<std::vector<Directive *>, std::vector<Directive *>>
createRealignRules(Statement *S, const uint64_t RegionId, File *F,
                   const std::string &TplCloneName,
                   const std::map<apc::Array *, std::set<apc::Array *>>
                       &ArrayLinksByFuncCalls,
                   const std::set<apc::Array *> &AccessedArrays,
                   const std::pair<int, int> Loc) {
  std::pair<std::vector<Directive *>, std::vector<Directive *>> Res;
  SmallVector<apc::Array *, 8> SortedArrays;
  for (auto *A : AccessedArrays) {
    if (A->IsNotDistribute())
      continue;
    SortedArrays.push_back(A);
  }
  sort(SortedArrays, [](auto &LHS, auto &RHS) {
    return LHS->GetShortName() < RHS->GetShortName();
  });
  auto LpStmt{cast<apc::LoopStatement>(S)};
  auto *Func{LpStmt->getFunction()};
  for (auto *A: SortedArrays) {
    auto *RealRef{getRealArrayRef(A, RegionId, ArrayLinksByFuncCalls)};
    auto Rules{RealRef->GetAlignRulesWithTemplate(RegionId)};
    auto Links{RealRef->GetLinksWithTemplate(RegionId)};
    auto *TplArray{RealRef->GetTemplateArray(RegionId)};
    auto makeRealign = [A, Func, &Links, &Rules,
                        DimSize = TplArray->GetDimSize()](Template *With) {
      auto Realign{std::make_unique<PragmaRealign>(
          A->GetDeclSymbol()->getVariable(Func).getValue(), A->GetDimSize(),
          With)};
      Realign->with().Relation.resize(DimSize);
      for (unsigned Dim = 0, DimE = A->GetDimSize(); Dim < DimE; ++Dim) {
        if (Links[Dim] != -1) {
          dvmh::Align::Axis Axis;
          Axis.Dimension = Dim;
          Axis.Step =
              APSInt{APInt{64, (uint64_t)Rules[Dim].first, true}, false};
          Axis.Offset =
              APSInt{APInt{64, (uint64_t)Rules[Dim].second, true}, false};
          Realign->with().Relation[Links[Dim]] = std::move(Axis);
        }
      }
      return Realign;
    };
    auto Tpl{TplArray->GetDeclSymbol()->getTemplate()};
    auto *TplClone{Tpl->getCloneOrCreate(TplCloneName)};
    Res.first.push_back(new DirectiveImpl{S, makeRealign(TplClone), Loc.first});
    Res.second.push_back(
        new DirectiveImpl{S, makeRealign(Tpl), Loc.second, nullptr, false});
  }
  return Res;
}

bool apc::LoopGraph::hasParalleDirectiveBefore() {
  return cast<apc::LoopStatement>(loop)->isScheduled();
}

void addRemoteLink(apc::ArrayRefExp *Expr,
                   std::map<std::string, apc::ArrayRefExp *> &UniqueRemotes,
                   const std::set<std::string> &RemotesInParallel,
                   std::set<ArrayRefExp *> &AddedRemotes,
                   std::vector<Messages> &Msgs, const int Line, bool WithConv) {
  auto *RemoteExpr{Expr};
  bool IsSimple{Expr->getArray()->GetDimSize() == Expr->size()};
  SmallString<128> RemoteExprStr{Expr->getArray()->GetShortName()};
  if (WithConv && IsSimple) {
    for (unsigned I = 0, EI = Expr->size(); I < EI; ++I)
      if (!Expr->isConstantSubscript(I)) {
        IsSimple = false;
        break;
      }
  }
  for (unsigned I = 0, EI = Expr->size(); I < EI; ++I) {
    RemoteExprStr += "[";
    if (IsSimple) {
      Expr->getConstantSubscript(I).toString(RemoteExprStr);
    }
    RemoteExprStr += "]";
  }
  auto REString{RemoteExprStr.str().str()};
  if (RemotesInParallel.count(REString))
    return;
  auto [Itr, IsNew] = UniqueRemotes.try_emplace(REString);
  if (!IsNew)
    return;
  if (WithConv) {
    RemoteExpr = new apc::ArrayRefExp{
        Expr->getContext(), Expr->getArray(),
        static_cast<std::size_t>(Expr->getArray()->GetDimSize()) };
    RemoteExpr->getContext()->addExpression(RemoteExpr);
    if (IsSimple)
      for (unsigned I = 0, EI = Expr->size(); I < EI; ++I)
        RemoteExpr->setConstantSubscript(I, Expr->getConstantSubscript(I));
  }
  Itr->second = RemoteExpr;
  AddedRemotes.insert(RemoteExpr);
}

apc::ArrayRefExp *createRemoteLink(const apc::Array *A) {
  auto DropConstA{const_cast<apc::Array *>(A)};
  auto Expr{new apc::ArrayRefExp(DropConstA->GetDeclSymbol()->getContext(),
                                 DropConstA, DropConstA->GetDimSize())};
  Expr->getContext()->addExpression(Expr);
  return Expr;
}
