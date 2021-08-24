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

#include "AstWrapperImpl.h"
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
#include "tsar/Transform/Clang/DVMHDirecitves.h"
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
using namespace tsar::dvmh;

namespace {
struct DirectiveImpl : public apc::Directive {
  explicit DirectiveImpl(apc::Statement *A, std::unique_ptr<ParallelItem> PI,
                         int ShrinkedLoc, bool IsOnEntry = true)
      : Anchor(A), Pragma(std::move(PI)), OnEntry(IsOnEntry) {
    bcl::restoreShrinkedPair(ShrinkedLoc, line, col);
  }

  apc::Statement *Anchor;
  bool OnEntry{true};
  std::unique_ptr<ParallelItem> Pragma;
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
  FileToFuncMap FileToFunc;
  ArrayAccessSummary ArrayRWs;
  ArrayAccessPool AccessPool;
  LoopToArrayMap Accesses;
  StringMap<std::map<int, apc::LoopGraph *>> Loops;
  DenseMap<apc::Statement *, BasicBlock *> APCToIRLoops;
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
      [&APCCtx, &APCToIRLoops, &LoopsForFile](Loop *L) {
        auto ID = L->getLoopID();
        if (!ID)
          return;
        if (auto APCLoop = APCCtx.findLoop(ID)) {
          LoopsForFile.emplace(APCLoop->lineNum, APCLoop);
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
  // Build data distribution.
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
  for (auto *A : AllArrays.GetArrays()) {
    if (A->IsTemplate()) {
      auto *Tpl{ParallelCtx.makeTemplate(A->GetShortName())};
      APCCtx.addArray(Tpl, A);
      auto APCSymbol{new apc::Symbol(Tpl)};
      APCCtx.addSymbol(APCSymbol);
      A->SetDeclSymbol(APCSymbol);
    }
  }
  LLVM_DEBUG(print(dbgs(), &M));
  // Build computation distribution.
  std::vector<apc::ParallelRegion *> Regions{ &APCRegion };
  std::vector<Messages> Msgs;
  for (auto &LoopsForFile : Loops) {
    if (LoopsForFile.second.empty())
      continue;
    createParallelDirectives(Accesses, Regions, LoopsForFile.second,
      FormalToActual, Msgs);
  }
  UniteNestedDirectives(OuterLoops);
  for (auto &LoopsForFile : Loops) {
    std::vector<apc::LoopGraph *> LoopList;
    for (auto &Info : LoopsForFile.second)
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
    selectParallelDirectiveForVariant(
        nullptr, &APCRegion, ReducedG, AllArrays, LoopList, LoopsForFile.second,
        Functions, CurrentVariant, DataDirs.alignRules, ParallelDirs,
        APCRegion.GetId(), FormalToActual, DepInfoForLoopGraph, Msgs);
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
        all_of(std::get<DirectiveList>(DirList),
               [](auto *Dir) { return Dir == nullptr; });
        assert(all_of(std::get<DirectiveList>(DirList),
                      [](auto *Dir) { return Dir == nullptr; }) &&
               "Memory leak!");
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
}

apc::Directive * ParallelDirective::genDirective(File *F,
    const std::vector<std::pair<apc::Array *, const DistrVariant *>>
        &Distribution,
    const std::vector<AlignRule> &AlignRules, apc::LoopGraph *CurrLoop,
    DIST::GraphCSR<int, double, attrType> &ReducedG,
    DIST::Arrays<int> &AllArrays,
    const uint64_t RegionId,
    const std::map<apc::Array *, std::set<apc::Array *>>
        &ArrayLinksByFuncCalls) {
  auto *DirImpl{new DirectiveImpl{
      CurrLoop->loop, std::make_unique<PragmaParallel>(), CurrLoop->lineNum}};
  auto *Parallel{cast<PragmaParallel>(DirImpl->Pragma.get())};
  const auto &ReadOps{CurrLoop->readOps};
  auto &Clauses{Parallel->getClauses()};
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
  Clauses.get<dvmh::Align>()->Target = APCSymbol->getMemory();
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
                            AllArrays, ReadOps, true, RegionId, Distribution,
                            ArraysInAcross, ShiftsByAccess,
                            ArrayLinksByFuncCalls)};
      if (Bounds.empty())
        continue;
      apc::Array *A{AllArrays.GetArrayByName(across[I].first.second)};
      assert(A && "Array must be known!");
      auto AcrossItr{
          Clauses.get<trait::Dependence>()
              .try_emplace(std::get<VariableT>(A->GetDeclSymbol()->getMemory()))
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
      auto Bounds{genBounds(AlignRules, shadowRenew[I], shadowRenewShifts[I],
                            ReducedG, AllArrays, ReadOps, false, RegionId,
                            Distribution, ArraysInAcross, ShiftsByAccess,
                            ArrayLinksByFuncCalls)};
      if (Bounds.empty())
        continue;
      apc::Array *A{AllArrays.GetArrayByName(shadowRenew[I].first.second)};
      assert(A && "Array must be known!");
      auto ShadowItr{
          Clauses.get<Shadow>()
              .try_emplace(std::get<VariableT>(A->GetDeclSymbol()->getMemory()))
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
  for (auto &A : AccessedArrays) {
    if (A->GetNonDistributeFlag())
      continue;
    auto *RealRef{getRealArrayRef(A, RegionId, ArrayLinksByFuncCalls)};
    auto Rules{RealRef->GetAlignRulesWithTemplate(RegionId)};
    auto Links{RealRef->GetLinksWithTemplate(RegionId)};
    auto *TplArray{RealRef->GetTemplateArray(RegionId)};
    auto makeRealign = [A, &Links, &Rules,
                        DimSize = TplArray->GetDimSize()](Template *With) {
      auto Realign{std::make_unique<PragmaRealign>(
          std::get<VariableT>(A->GetDeclSymbol()->getMemory()), A->GetDimSize(),
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
    auto Tpl{std::get<Template *>(TplArray->GetDeclSymbol()->getMemory())};
    auto *TplClone{Tpl->getCloneOrCreate(TplCloneName)};
    Res.first.push_back(new DirectiveImpl{S, makeRealign(TplClone), Loc.first});
    Res.second.push_back(
        new DirectiveImpl{S, makeRealign(Tpl), Loc.second, false});
  }
  return Res;
}
