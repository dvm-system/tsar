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

#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include "tsar/Support/SCEVUtils.h"
#include "tsar/Support/NumericUtils.h"
#include "tsar/Support/Diagnostic.h"
#include "Delinearization.h"
#include "DIEstimateMemory.h"
#include "GlobalOptions.h"
#include "KnownFunctionTraits.h"
#include "MemoryAccessUtils.h"
#include "tsar_pass_provider.h"
#include "tsar_utility.h"
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

namespace {
/// Pool of descriptions of arrays accesses.
///
/// We should use pointer to apc::ArrayInfo in APC library. However, it is not
/// necessary to allocate each apc::ArrayInfo separately because it has
/// a small size.
using ArrayAccessPool = std::vector<bcl::ValuePtrWrapper<apc::ArrayInfo>>;

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
};

using APCDataDistributionProvider = FunctionPassProvider<
  DominatorTreeWrapperPass,
  DelinearizationPass,
  LoopInfoWrapperPass,
  ScalarEvolutionWrapperPass>;

/// Convert representation of array access in LLVM IR to apc::ArrayInfo.
struct IRToArrayInfoFunctor {
  void operator()(Instruction &I, MemoryLocation &&Loc, unsigned OpIdx,
    AccessInfo IsRead, AccessInfo IsWrite);

  APCContext &APCCtx;
  const GlobalOptions &GlobalOpts;
  APCDataDistributionProvider &Provider;
  ArrayAccessPool &AccessPool;
  LoopToArrayMap &Accesses;
  apc::Array *APCArray;
  Array::Range &Range;
};
}

char APCDataDistributionPass::ID = 0;

INITIALIZE_PROVIDER_BEGIN(APCDataDistributionProvider, "apc-dd-provider",
  "Data Distribution Builder (APC, Provider")
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(DelinearizationPass)
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PROVIDER_END(APCDataDistributionProvider, "apc-dd-provider",
  "Data Distribution Builder (APC, Provider")

INITIALIZE_PASS_BEGIN(APCDataDistributionPass, "apc-data-distribution",
  "Data Distribution Builder (APC)", true, true)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCDataDistributionProvider)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_END(APCDataDistributionPass, "apc-data-distribution",
  "Data Distribution Builder (APC)", true, true)

ModulePass * llvm::createAPCDataDistributionPass() {
  return new APCDataDistributionPass;
}

void APCDataDistributionPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<APCContextWrapper>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
  AU.addRequired<APCDataDistributionProvider>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.setPreservesAll();
}

bool APCDataDistributionPass::runOnModule(Module &M) {
  auto &DL = M.getDataLayout();
  auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  auto &GlobalOpts = getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  auto &APCCtx = getAnalysis<APCContextWrapper>().get();
  APCDataDistributionProvider::initialize<GlobalOptionsImmutableWrapper>(
    [&GlobalOpts](GlobalOptionsImmutableWrapper &GO) {
      GO.setOptions(&GlobalOpts);
  });
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
  for (auto &F : M) {
    auto *FI = APCCtx.findFunction(F);
    if (!FI)
      continue;
    LLVM_DEBUG(dbgs() << "[APC DATA DISTRIBUTION]: process function "
                      << F.getName() << "\n");
    auto Itr = FileToFunc.emplace(std::piecewise_construct,
      std::forward_as_tuple(FI->fileName), std::forward_as_tuple()).first;
    Itr->second.push_back(FI);
    auto &Provider = getAnalysis<APCDataDistributionProvider>(F);
    auto &DT = Provider.get<DominatorTreeWrapperPass>().getDomTree();
    auto &DI = Provider.get<DelinearizationPass>().getDelinearizeInfo();
    for (auto *A : DI.getArrays()) {
      if (!A->isDelinearized() || !A->hasMetadata())
        continue;
      SmallVector<DIMemoryLocation, 4> DILocs;
      // TODO (kaniandr@gmail.com): add processing of array-members of structures.
      // Note, that delinearization of such array-member should be implemented
      // first. We can use GEP to determine which member of a structure
      // is accessed.
      auto DIM = findMetadata(A->getBase(), DILocs, &DT,
        A->isAddressOfVariable() ? MDSearch::AddressOfVariable : MDSearch::Any);
      assert(DIM && DIM->isValid() && "Metadata must be available for an array!");
      auto RawDIM = getRawDIMemoryIfExists(F.getContext(), *DIM);
      if (!RawDIM)
        continue;
      auto *APCArray = APCCtx.findArray(RawDIM);
      if (!APCArray)
        continue;
      auto &DeclInfo = *APCArray->GetDeclInfo().begin();
      auto PFIKey = PFIKeyT(DeclInfo.second, DeclInfo.first,
        APCArray->GetShortName());
      ArrayRWs.emplace(std::move(PFIKey), std::make_pair(APCArray, nullptr));
      LLVM_DEBUG(dbgs() << "[APC DATA DISTRIBUTION]: search for accesses to "
                        << APCArray->GetShortName() << "\n");
      for (auto &Range : *A) {
        if (!Range.isValid())
          continue;
        for (auto *U : Range.Ptr->users()) {
          if (!isa<Instruction>(U))
            continue;
          LLVM_DEBUG(dbgs() << "[APC DATA DISTRIBUTION]: access at ";
            cast<Instruction>(U)->getDebugLoc().dump(); dbgs() << "\n");
          for_each_memory(cast<Instruction>(*U), TLI,
            IRToArrayInfoFunctor{APCCtx, GlobalOpts, Provider,
              AccessPool, Accesses, APCArray, Range },
            [](Instruction &I, AccessInfo IsRead, AccessInfo IsWrite) {});
        }
      }
    }
  }
  FormalToActualMap FormalToActual;
  createLinksBetweenFormalAndActualParams(FileToFunc, FormalToActual, ArrayRWs);
  processLoopInformationForFunction(Accesses);
  addToDistributionGraph(Accesses, FormalToActual);
  auto &APCRegion = APCCtx.getDefaultRegion();
  auto &G = APCRegion.GetGraphToModify();
  auto &ReducedG = APCRegion.GetReducedGraphToModify();
  auto &AllArrays = APCRegion.GetAllArraysToModify();
  createOptimalDistribution(G, ReducedG, AllArrays, APCRegion.GetId(), false);
  // TODO (kaniandr@gmail.com): should it be a global container?
  std::map<std::string, std::vector<Messages>> APCMsgs;
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

void IRToArrayInfoFunctor::operator()(Instruction &I, MemoryLocation &&Loc,
    unsigned OpIdx, AccessInfo IsRead, AccessInfo IsWrite) {
  if (Loc.Ptr != Range.Ptr)
    return;
  auto &LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
  auto &SE = Provider.get<ScalarEvolutionWrapperPass>().getSE();
  for (std::size_t DimIdx = 0, DimIdxE = Range.Subscripts.size();
       DimIdx < DimIdxE; ++DimIdx) {
    auto *S = Range.Subscripts[DimIdx];
    auto AddRecInfo = computeSCEVAddRec(S, SE);
    const SCEV *Coef, *ConstTerm;
    const Loop *L = nullptr;
    if ((!GlobalOpts.IsSafeTypeCast || AddRecInfo.second) &&
        isa<SCEVAddRecExpr>(AddRecInfo.first)) {
      auto AddRec = cast<SCEVAddRecExpr>(AddRecInfo.first);
      Coef = AddRec->getStepRecurrence(SE);
      ConstTerm = AddRec->getStart();
      L = AddRec->getLoop();
    } else {
      Coef = SE.getZero(S->getType());
      ConstTerm = S;
    }
    if (!L || !isa<SCEVConstant>(Coef) || !isa<SCEVConstant>(ConstTerm))
      return;
    decltype(std::declval<apc::ArrayOp>().coefficients)::key_type ABPair;
    if (!castSCEV(Coef, true, ABPair.first)) {
      emitTypeOverflow(I.getContext(), *I.getFunction(), I.getDebugLoc(),
        "unable to represent A constant in A*I + B subscript", DS_Warning);
        return;
    }
    if (!castSCEV(ConstTerm, true, ABPair.second)) {
      emitTypeOverflow(I.getContext(), *I.getFunction(), I.getDebugLoc(),
        "unable to represent B constant in A*I + B subscript", DS_Warning);
        return;
    }
    assert(L->getLoopID() && "Loop ID must not be null!");
    auto *APCLoop = APCCtx.findLoop(L->getLoopID());
    auto &APCLoopAccesses = Accesses.emplace(std::piecewise_construct,
      std::forward_as_tuple(APCLoop), std::forward_as_tuple()).first->second;
    auto &APCArrayAccessesC =
      APCLoopAccesses.emplace(APCArray, nullptr).first->second;
    if (!APCArrayAccessesC) {
      AccessPool.emplace_back(APCArrayAccessesC);
      auto &Info = AccessPool.back();
      Info->dimSize = Range.Subscripts.size();
      Info->readOps.resize(Info->dimSize);
      Info->writeOps.resize(Info->dimSize);
      Info->unrecReadOps.resize(Info->dimSize);
    }
    APCArray->SetMappedDim(DimIdx);
    auto *APCArrayAccesses = const_cast<ArrayInfo *>(APCArrayAccessesC);
    LLVM_DEBUG(dbgs() << "[APC DATA DISTRIBUTION]: dimension " << DimIdx
                      << " subscript " << ABPair.first << " * I + "
                      << ABPair.second << " access type";
               if (IsWrite != AccessInfo::No) dbgs() << " write";
               if (IsRead != AccessInfo::No) dbgs() << " read"; dbgs() << "\n");
    if (IsWrite != AccessInfo::No)
      APCArrayAccesses->writeOps[DimIdx].coefficients.emplace(ABPair, 1.0);
    if (IsRead != AccessInfo::No)
      APCArrayAccesses->readOps[DimIdx].coefficients.emplace(ABPair, 1.0);
  }
}
