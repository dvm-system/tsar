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

#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Memory/Delinearization.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
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
#include <apc/DirectiveAnalyzer/DirectiveAnalyzer.h>
#include <apc/Distribution/CreateDistributionDirs.h>
#include <apc/GraphCall/graph_calls.h>
#include <apc/GraphCall/graph_calls_func.h>
#include <apc/GraphLoop/graph_loops.h>
#include <apc/LoopAnalyzer/directive_creator.h>
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

namespace {
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
  std::map<apc::LoopGraph *, std::map<apc::Array*, const apc::ArrayInfo*>>;

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
}

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
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCDataDistributionProvider)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_IN_GROUP_END(APCDataDistributionPass, "apc-data-distribution",
  "Data Distribution Builder (APC)", true, true,
  DefaultQueryManager::PrintPassGroup::getPassRegistry())

ModulePass * llvm::createAPCDataDistributionPass() {
  return new APCDataDistributionPass;
}

void APCDataDistributionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<APCDataDistributionProvider>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAccessWrapper>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.addRequired<DIArrayAccessWrapper>();
  AU.setPreservesAll();
}

bool APCDataDistributionPass::runOnModule(Module &M) {
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
  FileToFuncMap FileToFunc;
  ArrayAccessSummary ArrayRWs;
  ArrayAccessPool AccessPool;
  LoopToArrayMap Accesses;
  StringMap<std::map<int, apc::LoopGraph *>> Loops;
  std::map<std::string, apc::FuncInfo *> Functions;
  std::vector<apc::LoopGraph *> OuterLoops;
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
    auto &DT = Provider.get<DominatorTreeWrapperPass>().getDomTree();
    auto &DI = Provider.get<DelinearizationPass>().getDelinearizeInfo();
    auto &AT = Provider.get<EstimateMemoryPass>().getAliasTree();
    auto &DIAT = Provider.get<DIEstimateMemoryPass>().getAliasTree();
    auto &LoopsForFile = Loops.try_emplace(FI->fileName).first->second;
    for_each_loop(Provider.get<LoopInfoWrapperPass>().getLoopInfo(),
      [&APCCtx, &LoopsForFile](Loop *L) {
        auto ID = L->getLoopID();
        if (!ID)
          return;
        if (auto APCLoop = APCCtx.findLoop(ID))
          LoopsForFile.emplace(APCLoop->lineNum, APCLoop);
      });
    for (auto *L : Provider.get<LoopInfoWrapperPass>().getLoopInfo()) {
      auto ID = L->getLoopID();
      if (!ID)
        continue;
      if (auto APCLoop = APCCtx.findLoop(ID))
        OuterLoops.push_back(APCLoop);
    }
    for (auto &Access : *DIArrayInfo) {
      for (unsigned DimIdx = 0, DimIdxE = Access.size(); DimIdx < DimIdxE;
           ++DimIdx) {
        auto AffineAccess =
            dyn_cast_or_null<DIAffineSubscript>(Access[DimIdx]);
        if (!AffineAccess)
          continue;
        if (AffineAccess->getNumberOfMonoms() != 1)
          continue;
        auto *APCArray = APCCtx.findArray(Access.getArray()->getAsMDNode());
        if (!APCArray)
          continue;
        auto &DeclInfo = *APCArray->GetDeclInfo().begin();
        auto PFIKey = PFIKeyT(DeclInfo.second, DeclInfo.first,
          APCArray->GetShortName());
        ArrayRWs.emplace(std::move(PFIKey), std::make_pair(APCArray, nullptr));
        LLVM_DEBUG(dbgs() << "[APC DATA DISTRIBUTION]: search for accesses to "
          << APCArray->GetShortName() << "\n");

        decltype(std::declval<apc::ArrayOp>().coefficients)::key_type ABPair;
        if (auto C{AffineAccess->getMonom(0).Value};
            C.Kind != DIAffineSubscript::Symbol::SK_Constant ||
            !castAPInt(C.Constant, true, ABPair.first)) {
          emitTypeOverflow(F.getContext(), F, DebugLoc(), //TODO
            "unable to represent A constant in A*I + B subscript", DS_Warning);
          continue;
        }
        if (AffineAccess->getSymbol().Kind !=
                DIAffineSubscript::Symbol::SK_Constant ||
            !castAPInt(AffineAccess->getSymbol().Constant, true,
                       ABPair.second)) {
          emitTypeOverflow(
              F.getContext(), F, DebugLoc(), // TODO
              "unable to represent B constant in A*I + B subscript",
              DS_Warning);
          continue;
        }
        auto *APCLoop = APCCtx.findLoop(AffineAccess->getMonom(0).Column);
        auto &APCLoopAccesses = Accesses.emplace(std::piecewise_construct,
          std::forward_as_tuple(APCLoop), std::forward_as_tuple()).first->second;
        auto &APCArrayAccessesC =
          APCLoopAccesses.emplace(APCArray, nullptr).first->second;
        if (!APCArrayAccessesC) {
          AccessPool.emplace_back(APCArrayAccessesC);
          auto &Info = AccessPool.back();
          Info->dimSize = APCArray->GetDimSize();
          Info->readOps.resize(Info->dimSize);
          Info->writeOps.resize(Info->dimSize);
          Info->unrecReadOps.resize(Info->dimSize);
        }
        APCArray->SetMappedDim(AffineAccess->getDimension());
        auto *APCArrayAccesses = const_cast<ArrayInfo *>(APCArrayAccessesC);
        LLVM_DEBUG(dbgs() << "[APC DATA DISTRIBUTION]: dimension " << DimIdx
          << " subscript " << ABPair.first << " * I + "
          << ABPair.second << " access type";
        if (!Access.isReadOnly()) dbgs() << " write";
        if (!Access.isWriteOnly()) dbgs() << " read";
        dbgs() << "\n");
        if (!Access.isReadOnly())
          APCArrayAccesses->writeOps[DimIdx].coefficients.emplace(ABPair, 1.0);
        if (!Access.isWriteOnly())
          APCArrayAccesses->readOps[DimIdx].coefficients.emplace(ABPair, 1.0);
      }
    }
  }
  // TODO (kaniandr@gmail.com): should it be a global container?
  std::map<std::string, std::vector<Messages>> APCMsgs;
  FormalToActualMap FormalToActual;
  createLinksBetweenFormalAndActualParams(
    FileToFunc, FormalToActual, ArrayRWs, APCMsgs);
  processLoopInformationForFunction(Accesses);
  addToDistributionGraph(Accesses, FormalToActual);
  auto &G = APCRegion.GetGraphToModify();
  auto &ReducedG = APCRegion.GetReducedGraphToModify();
  auto &AllArrays = APCRegion.GetAllArraysToModify();
  createOptimalDistribution(G, ReducedG, AllArrays, APCRegion.GetId(), false);
  auto &DataDirs = APCRegion.GetDataDirToModify();
  createDistributionDirs(
    ReducedG, AllArrays, DataDirs, APCMsgs, FormalToActual);
  createAlignDirs(
    ReducedG, AllArrays, DataDirs, APCRegion.GetId(), FormalToActual, APCMsgs);
  std::vector<int> FullDistrVariant;
  FullDistrVariant.reserve(DataDirs.distrRules.size());
  for (auto TplInfo : DataDirs.distrRules)
    FullDistrVariant.push_back(TplInfo.second.size() - 1);
  APCRegion.SetCurrentVariant(std::move(FullDistrVariant));
  LLVM_DEBUG(print(dbgs(), &M));
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
}
