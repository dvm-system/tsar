//===- Parallelization.cpp - Core Parallelization Engine --------*- C++ -*-===//
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
// This file implements a core pass to perform DVMH-based parallelization with
// data distribution.
//
//===----------------------------------------------------------------------===//

#include "APCContextImpl.h"
#include "AstWrapperImpl.h"
#include "DistributionUtils.h"
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
#include <apc/Distribution/DvmhDirectiveBase.h>
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
#define DEBUG_TYPE "apc-parallelization"

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

/// Pool of descriptions of arrays accesses.
///
/// We should use pointer to apc::ArrayInfo in APC library. However, it is not
/// necessary to allocate each apc::ArrayInfo separately because it has
/// a small size.
using ArrayAccessPool = std::vector<bcl::ValuePtrWrapper<apc::ArrayInfo>>;

/// Map from a file name to a list of functions which are located in a file.
using FileToFuncMap = std::map<std::string, std::vector<apc::FuncInfo *>>;

/// Map from a file name to a list of loops which are located in a file.
using FileToLoopMap = std::map<std::string, std::vector<apc::LoopGraph *>>;

/// Map from a file name to a list of messages emitted for this file.
using FileToMessageMap = APCContextImpl::FileDiagnostics;

/// Map from a function to a list of top level loops.
using FuncToLoopMap = DenseMap<
    apc::FuncInfo *, std::tuple<std::vector<apc::LoopGraph *>>,
    DenseMapInfo<apc::FuncInfo *>,
    TaggedDenseMapTuple<bcl::tagged<apc::FuncInfo *, apc::FuncInfo>,
                        bcl::tagged<std::vector<apc::LoopGraph *>, Root>>>;

/// Map from a loop to a set of accessed arrays and descriptions of accesses.
using LoopToArrayMap =
    std::map<apc::LoopGraph *, std::map<apc::Array *, apc::ArrayInfo *>>;

/// Map from a file name to a necessary information.
using FileInfoMap = StringMap<bcl::tagged_tuple<
    bcl::tagged<std::map<int, apc::LoopGraph *>, apc::LoopGraph>,
    bcl::tagged<std::vector<apc::ArrayRefExp *>, apc::ArrayRefExp>,
    bcl::tagged<std::vector<apc::LoopGraph *>, Root>,
    bcl::tagged<LoopToArrayMap, LoopToArrayMap>>>;

/// Map from a function name to a corresponding function representation.
using NameToFunctionMap = std::map<std::string, apc::FuncInfo *>;

/// Map from a formal parameter to a list of possible actual parameters.
using FormalToActualMap = std::map<apc::Array *, std::set<apc::Array *>>;

/// Unique key which is <Position, File, Identifier>.
using PFIKeyT = std::tuple<int, std::string, std::string>;
using KeyToArrayMap = std::map<PFIKeyT, apc::Array *>;
using ArrayToKeyMap = std::map<apc::Array *, PFIKeyT>;

/// Map from unique array identifier to a summary of array accesses.
///
/// This map does not contains arrays which are not accessed (read or write)
/// in a program.
///
/// TODO (kaniandr@gmail.com): at this moment apc::ArrayAccessInfo is not
/// used, so it is initialized with `nullptr`.
using ArrayAccessSummary =
    std::map<PFIKeyT, std::pair<apc::Array *, apc::ArrayAccessInfo *>>;

/// List of regions in a source code which should be parallelized.
using RegionList = std::vector<apc::ParallelRegion *>;

/// Map from a APC-level loop representation to its head in the IR-level
/// representation.
using APCToIRLoopsMap = DenseMap<apc::Statement *, BasicBlock *>;

class APCParallelizationPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  APCParallelizationPass() : ModulePass(ID) {
    initializeAPCParallelizationPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  void collectFunctionsAndLoops(
      Module &M, APCContext &APCCtx, NameToFunctionMap &Functions,
      FileToFuncMap &FileToFunc, FileInfoMap &Files, FileToLoopMap &FileToLoop,
      FuncToLoopMap &FuncToLoop, APCToIRLoopsMap &APCToIRLoops);

  void collectArrayAccessInfo(
      Module &M, DIArrayAccess &Access, APCContext &APCCtx,
      ArrayAccessPool &AccessPool, FileInfoMap &Files, ArrayToKeyMap &ArrayIds);

  void bindFormalAndActualPrameters(
    const ArrayAccessSummary &ArrayRWs, FileToFuncMap FileToFunc,
    FormalToActualMap &FormalToActual, FileToMessageMap &APCMsgs);

  void buildDataDistributionGraph(
      const RegionList &Regions,  const FormalToActualMap &FormalToActual,
      const ArrayAccessSummary &ArrayRWs, const FileToFuncMap &FileToFunc,
      const FuncToLoopMap &FuncToLoop, FileToLoopMap &FileToLoop,
      FileInfoMap &Files, KeyToArrayMap &CreatedArrays,
      FileToMessageMap &APCMsgs);

  void buildDataDistribution(
      const RegionList &Regions, const FormalToActualMap &FormalToActual,
      const ArrayToKeyMap &ArrayIds, const FileInfoMap &Files,
      APCContext &APCCtx, DVMHParallelizationContext &ParallelCtx,
      std::set<apc::Array *> &DistributedArrays, KeyToArrayMap &CreatedArrays,
      FileToMessageMap &APCMsgs);

  void selectComputationDistribution(
      const FormalToActualMap &FormalToActual,
      const NameToFunctionMap &Functions, FileInfoMap::value_type &FileInfo,
      apc::ParallelRegion &APCRegion,
      std::vector<apc::Directive *> &ParallelDirs, FileToMessageMap &APCMsgs);

  void updateParallelization(APCToIRLoopsMap &APCToIRLoops,
      const FormalToActualMap &FormalToActual,
      const FileInfoMap::value_type &FileInfo, const NameToFunctionMap &Functions,
      apc::ParallelRegion &APCRegion, DVMHParallelizationContext &ParallelCtx,
      std::vector<apc::Directive *> &ParallelDirs, FileToMessageMap &APCMsgs);

  void addRemoteAccessDirectives(
      Function &ClientF, const std::set<apc::Array *> &DistributedArrays,
      APCContext &APCCtx, DVMHParallelizationContext &ParallelCtx);

  void printDistribution(raw_ostream &OS) const;
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

char APCParallelizationPass::ID = 0;

INITIALIZE_PROVIDER_BEGIN(APCDataDistributionProvider,
                          "apc-parallelization-provider",
                          "DVMH-based Parallelization (APC, Provider)")
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DelinearizationPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PROVIDER_END(APCDataDistributionProvider,
                        "apc-parallelization-provider",
                        "DVMH-based Parallelization (APC, Provider)")

INITIALIZE_PASS_BEGIN(APCParallelizationPass, "apc-parallelization",
  "DVMH-based Parallelization (APC)", true, true)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
  INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(APCDataDistributionProvider)
  INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
  INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
  INITIALIZE_PASS_DEPENDENCY(DVMHParallelizationContext)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_END(APCParallelizationPass, "apc-parallelization",
  "DVMH-based Parallelization (APC)", true, true)

ModulePass * llvm::createAPCParallelizationPass() {
  return new APCParallelizationPass;
}

void APCParallelizationPass::getAnalysisUsage(AnalysisUsage &AU) const {
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

void APCParallelizationPass::collectFunctionsAndLoops(Module &M,
  APCContext &APCCtx, NameToFunctionMap &Functions, FileToFuncMap &FileToFunc,
  FileInfoMap &Files, FileToLoopMap &FileToLoop, FuncToLoopMap &FuncToLoop,
  APCToIRLoopsMap &APCToIRLoops) {
  for (auto &F : M) {
    auto *FI{APCCtx.findFunction(F)};
    if (!FI)
      continue;
    LLVM_DEBUG(dbgs() << "[APC]: process function " << F.getName() << "\n");
    Functions.emplace(FI->funcName, FI);
    auto Itr{FileToFunc
                 .emplace(std::piecewise_construct,
                          std::forward_as_tuple(FI->fileName),
                          std::forward_as_tuple())
                 .first};
    Itr->second.push_back(FI);
    auto &Provider{getAnalysis<APCDataDistributionProvider>(F)};
    auto &FileInfo{Files.try_emplace(FI->fileName).first->second};
    auto &LoopList{FileToLoop.try_emplace(FI->fileName).first->second};
    auto &FuncLoopList{*FuncToLoop.try_emplace(FI).first};
    for_each_loop(
        Provider.get<LoopInfoWrapperPass>().getLoopInfo(),
        [&APCCtx, &FileInfo, &LoopList, &APCToIRLoops, &FuncLoopList](Loop *L) {
          if (auto ID{L->getLoopID()})
            if (auto APCLoop{APCCtx.findLoop(ID)}) {
              FileInfo.get<apc::LoopGraph>().emplace(APCLoop->lineNum, APCLoop);
              LoopList.push_back(APCLoop);
              APCToIRLoops.try_emplace(APCLoop->loop, L->getHeader());
              if (!APCLoop->parent) {
                FileInfo.get<Root>().push_back(APCLoop);
                FuncLoopList.get<Root>().push_back(APCLoop);
              }
            }
        });
  }
}

void APCParallelizationPass::collectArrayAccessInfo(
    Module &M, DIArrayAccess &Access, APCContext &APCCtx,
    ArrayAccessPool &AccessPool, FileInfoMap &Files, ArrayToKeyMap &ArrayIds) {
  auto findOrInsert = [&AccessPool](apc::LoopGraph *APCLoop,
                                    apc::Array *APCArray,
                                    LoopToArrayMap &LToA) {
    auto &APCLoopAccesses{LToA.emplace(std::piecewise_construct,
                                       std::forward_as_tuple(APCLoop),
                                       std::forward_as_tuple())
                              .first->second};
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
  auto registerUnknownRead = [](apc::Array *APCArray,
                                unsigned NumberOfSubscripts, unsigned DimIdx,
                                apc::ArrayRefExp *Expr,
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
  auto *APCArray{APCCtx.findArray(Access.getArray()->getAsMDNode())};
  if (!APCArray)
    return;
  auto *Scope{APCCtx.findLoop(Access.getParent())};
  if (!Scope)
    return;
  auto *FI{APCCtx.findFunction(
      *cast<apc::LoopStatement>(Scope->loop)->getFunction())};
  assert(FI && "Function must be registered!");
  auto &FileInfo{Files[FI->fileName]};
  apc::ArrayRefExp *AccessExpr{nullptr};
  if (!Access.isWriteOnly()) {
    AccessExpr = new apc::ArrayRefExp{&APCCtx, Scope, APCArray, Access.size()};
    APCCtx.addExpression(AccessExpr);
    LLVM_DEBUG(dbgs() << "[APC]: register immediate access in loop at "
                      << Scope->lineNum << "\n");
    cast<apc::LoopStatement>(Scope->loop)->addImmediateAccess(AccessExpr);
    FileInfo.get<apc::ArrayRefExp>().push_back(AccessExpr);
  }
  LLVM_DEBUG(dbgs() << "[APC]: explore an access to "
                    << APCArray->GetShortName() << " in loop at "
                    << Scope->lineNum << "\n");
  auto skipAccess = [APCArray](apc::LoopGraph *APCLoop) {
    if (!APCArray->IsNotDistribute())
      return false;
    auto *LpStmt{cast<apc::LoopStatement>(APCLoop->loop)};
    assert(LpStmt && "IR-level description of a loop must not be null!");
    auto Var{APCArray->GetDeclSymbol()->getVariable(LpStmt->getFunction())};
    assert(Var && "Variable must not be null!");
    if (LpStmt->getTraits().get<trait::Private>().count(*Var) ||
        LpStmt->getTraits().get<trait::Local>().count(*Var))
      return true;
    return false;
  };
  if (Access.empty()) {
    auto *APCLoop{Scope};
    while (APCLoop) {
      if (!skipAccess(APCLoop)) {
        APCLoop->withoutDistributedArrays &= APCArray->IsNotDistribute();
        APCLoop->hasUnknownDistributedMap = true;
        if (!Access.isWriteOnly()) {
          assert(AccessExpr && "Expression must not be null!");
          for (unsigned DimIdx = 0, DimIdxE = APCArray->GetDimSize();
               DimIdx < DimIdxE; ++DimIdx)
            registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                                findOrInsert(APCLoop, APCArray,
                                             FileInfo.get<LoopToArrayMap>()));
        }
        if (!Access.isReadOnly()) {
          APCLoop->hasUnknownArrayAssigns = true;
          APCLoop->hasWritesToNonDistribute |= APCArray->IsNotDistribute();
        }
      }
      APCLoop = APCLoop->parent;
    }
    return;
  }
  SmallDenseMap<apc::LoopGraph *, uint8_t, 8> NumberOfDimsForLoop;
  for (unsigned DimIdx = 0, DimIdxE = Access.size(); DimIdx < DimIdxE;
       ++DimIdx) {
    auto AffineAccess = dyn_cast_or_null<DIAffineSubscript>(Access[DimIdx]);
    if (!AffineAccess) {
      if (!Access.isWriteOnly()) {
        auto *APCLoop{Scope};
        while (APCLoop) {
          if (!skipAccess(APCLoop)) {
            assert(AccessExpr && "Expression must not be null!");
            registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                                findOrInsert(APCLoop, APCArray,
                                             FileInfo.get<LoopToArrayMap>()));
          }
          APCLoop = APCLoop->parent;
        }
      }
      continue;
    }
    if (AffineAccess->getNumberOfMonoms() != 1) {
      for (unsigned I = 0, EI = AffineAccess->getNumberOfMonoms(); I < EI;
           ++I) {
        auto &Monom{AffineAccess->getMonom(I)};
        auto *APCLoop{APCCtx.findLoop(Monom.Column)};
        ++NumberOfDimsForLoop.try_emplace(APCLoop, 0).first->second;
      }
      if (!Access.isWriteOnly()) {
        if (AffineAccess->getNumberOfMonoms() == 0) {
          if (auto &S{AffineAccess->getSymbol()};
              S.Kind == DIAffineSubscript::Symbol::SK_Constant)
            AccessExpr->setConstantSubscript(DimIdx, S.Constant);
          auto *APCLoop{Scope};
          while (APCLoop) {
            if (!skipAccess(APCLoop)) {
              assert(AccessExpr && "Expression must not be null!");
              registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                                  findOrInsert(APCLoop, APCArray,
                                               FileInfo.get<LoopToArrayMap>()));
            }
            APCLoop = APCLoop->parent;
          }
        } else {
          for (unsigned I = 0, EI = AffineAccess->getNumberOfMonoms(); I < EI;
               ++I) {
            auto &Monom{AffineAccess->getMonom(I)};
            auto *APCLoop{APCCtx.findLoop(Monom.Column)};
            if (skipAccess(APCLoop))
              continue;
            assert(AccessExpr && "Expression must not be null!");
            AccessExpr->registerRecInDim(DimIdx, APCLoop);
            registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                                findOrInsert(APCLoop, APCArray,
                                             FileInfo.get<LoopToArrayMap>()));
          }
        }
      }
      continue;
    }
    auto *APCLoop{APCCtx.findLoop(AffineAccess->getMonom(0).Column)};
    ++NumberOfDimsForLoop.try_emplace(APCLoop, 0).first->second;
    if (skipAccess(APCLoop))
      continue;
    auto LpStmt{cast<apc::LoopStatement>(APCLoop->loop)};
    decltype(std::declval<apc::ArrayOp>().coefficients)::key_type ABPair;
    if (auto C{AffineAccess->getMonom(0).Value};
        C.Kind != DIAffineSubscript::Symbol::SK_Constant &&
            (C.Kind != DIAffineSubscript::Symbol::SK_Induction ||
             C.Variable != LpStmt->getInduction().get<MD>()) ||
        !castAPInt(C.Constant, true, ABPair.first)) {
      if (!Access.isWriteOnly()) {
        assert(AccessExpr && "Expression must not be null!");
        AccessExpr->registerRecInDim(DimIdx, APCLoop);
        registerUnknownRead(
            APCArray, DimIdxE, DimIdx, AccessExpr,
            findOrInsert(APCLoop, APCArray, FileInfo.get<LoopToArrayMap>()));
      }
      std::wstring MsgEn, MsgRu;
      __spf_printToLongBuf(
          MsgEn,
          L"unable to represent A constant in A*I + B subscript in "
          L"access to %s",
          to_wstring(APCArray->GetShortName()).c_str());
      __spf_printToLongBuf(MsgRu, R57,
                           to_wstring(APCArray->GetShortName()).c_str());
      getObjectForFileFromMap(APCLoop->fileName.c_str(), APCCtx.mImpl->Diags)
          .push_back(Messages{WARR, APCLoop->lineNum, MsgRu, MsgEn, 1023});
      continue;
    }
    if (AffineAccess->getSymbol().Kind !=
                DIAffineSubscript::Symbol::SK_Constant &&
            (AffineAccess->getSymbol().Kind !=
                 DIAffineSubscript::Symbol::SK_Induction ||
             AffineAccess->getSymbol().Variable !=
                 LpStmt->getInduction().get<MD>()) ||
        !castAPInt(AffineAccess->getSymbol().Constant, true, ABPair.second)) {
      if (!Access.isWriteOnly()) {
        assert(AccessExpr && "Expression must not be null!");
        AccessExpr->registerRecInDim(DimIdx, APCLoop);
        registerUnknownRead(
            APCArray, DimIdxE, DimIdx, AccessExpr,
            findOrInsert(APCLoop, APCArray, FileInfo.get<LoopToArrayMap>()));
      }
      std::wstring MsgEn, MsgRu;
      __spf_printToLongBuf(
          MsgEn,
          L"unable to represent B constant in A*I + B subscript in "
          L"access to %s",
          to_wstring(APCArray->GetShortName()).c_str());
      __spf_printToLongBuf(MsgRu, R57,
                           to_wstring(APCArray->GetShortName()).c_str());
      getObjectForFileFromMap(APCLoop->fileName.c_str(), APCCtx.mImpl->Diags)
          .push_back(Messages{WARR, APCLoop->lineNum, MsgRu, MsgEn, 1023});
      continue;
    }
    APCArray->SetMappedDim(AffineAccess->getDimension());
    auto *APCArrayAccesses{
        findOrInsert(APCLoop, APCArray, FileInfo.get<LoopToArrayMap>())};
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
      AccessExpr->setAffineSubscript(
          DimIdx, {AffineAccess->getMonom(0).Value.Constant, APCLoop,
                   AffineAccess->getSymbol().Constant});
      registerUnknownRead(APCArray, DimIdxE, DimIdx, AccessExpr,
                          APCArrayAccesses, REMOTE_FALSE);
      auto [Itr, IsNew] = APCArrayAccesses->arrayAccess.try_emplace(AccessExpr);
      if (IsNew) {
        Itr->second.first = 0;
        Itr->second.second.resize(Access.size());
      }
      Itr->second.second[DimIdx].coefficients.emplace(ABPair, 1.0);
    }
  }
  auto *APCLoop{Scope};
  while (APCLoop) {
    if (!skipAccess(APCLoop)) {
      APCLoop->withoutDistributedArrays &= APCArray->IsNotDistribute();
      APCLoop->hasWritesToNonDistribute |=
          !Access.isReadOnly() && APCArray->IsNotDistribute();
      if (!APCLoop->hasUnknownDistributedMap ||
          (!APCLoop->hasUnknownArrayAssigns && !Access.isReadOnly())) {
        auto Itr{NumberOfDimsForLoop.find(APCLoop)};
        APCLoop->hasUnknownArrayAssigns |=
            (!Access.isReadOnly() &&
             (Itr == NumberOfDimsForLoop.end() || Itr->second > 1));
        APCLoop->hasUnknownDistributedMap |=
            (Itr == NumberOfDimsForLoop.end() || Itr->second > 1);
      }
    }
    APCLoop = APCLoop->parent;
  }
}

void APCParallelizationPass::bindFormalAndActualPrameters(
    const ArrayAccessSummary &ArrayRWs, FileToFuncMap FileToFunc,
    FormalToActualMap &FormalToActual, FileToMessageMap &APCMsgs) {
  bool IsDebug{false};
  LLVM_DEBUG(IsDebug = true);
  createLinksBetweenFormalAndActualParams(FileToFunc, FormalToActual,
                                          ArrayRWs, APCMsgs, IsDebug);
  LLVM_DEBUG(dbgs() << "[APC]: formal to actual parameters correspondence:\n";
             for (auto &[Formal, ActualList]
                  : FormalToActual) {
               dbgs() << Formal->GetName() << " -> ";
               for (auto *A : ActualList)
                 dbgs() << A->GetName() << " ";
               dbgs() << "\n";
             });
}

void APCParallelizationPass::buildDataDistributionGraph(
    const RegionList &Regions, const FormalToActualMap &FormalToActual,
    const ArrayAccessSummary &ArrayRWs, const FileToFuncMap &FileToFunc,
    const FuncToLoopMap &FuncToLoop, FileToLoopMap &FileToLoop,
    FileInfoMap &Files, KeyToArrayMap &CreatedArrays,
    FileToMessageMap &APCMsgs) {
  // Build a data distribution graph.
  for (auto &FileInfo : Files) {
    auto &Accesses{FileInfo.second.get<LoopToArrayMap>()};
    processLoopInformationForFunction(Accesses);
    addToDistributionGraph(Accesses, FormalToActual);
  }
  excludeArraysFromDistribution(FormalToActual, ArrayRWs, FileToLoop,
                                Regions, APCMsgs, CreatedArrays);
  for (auto &FileInfo : Files) {
    auto &Accesses{FileInfo.second.get<LoopToArrayMap>()};
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
      else
        ++LpI;
    }
  }
  // Rebuild the data distribution graph without excluded arrays.
  for (auto &FileInfo : Files) {
    auto &Accesses{FileInfo.second.get<LoopToArrayMap>()};
    processLoopInformationForFunction(Accesses);
    addToDistributionGraph(Accesses, FormalToActual);
    if (auto FuncItr{FileToFunc.find(FileInfo.getKey().str())};
        FuncItr != FileToFunc.end())
      for (auto *FI : FuncItr->second)
        if (auto LoopItr{FuncToLoop.find(FI)}; LoopItr != FuncToLoop.end())
          selectFreeLoopsForParallelization(
              LoopItr->get<Root>(), FI->funcName, true, Regions,
              getObjectForFileFromMap(FileInfo.getKeyData(), APCMsgs));
  }
}

#ifdef LLVM_DEBUG
static void dotGraphLog(apc::ParallelRegion &APCRegion) {
  auto &ReducedG{APCRegion.GetReducedGraphToModify()};
  auto &AllArrays{APCRegion.GetAllArraysToModify()};
  SmallString<256> FName{"graph_reduced_with_templ_reg"};
  Twine(APCRegion.GetId()).toVector(FName);
  FName += ".dot";
  ReducedG.CreateGraphWiz(FName.c_str(),
                          std::vector<std::tuple<int, int, attrType>>(),
                          AllArrays, true);
}
#endif

void APCParallelizationPass::buildDataDistribution(const RegionList &Regions,
    const FormalToActualMap &FormalToActual, const ArrayToKeyMap &ArrayIds,
    const FileInfoMap &Files, APCContext &APCCtx,
    DVMHParallelizationContext &ParallelCtx,
    std::set<apc::Array *> &DistributedArrays, KeyToArrayMap &CreatedArrays,
    FileToMessageMap &APCMsgs) {
  std::set<apc::Array *> ArraysInAllRegions;
  for (auto *APCRegion : Regions) {
    auto &G{APCRegion->GetGraphToModify()};
    auto &ReducedG{APCRegion->GetReducedGraphToModify()};
    auto &AllArrays{APCRegion->GetAllArraysToModify()};
    ArraysInAllRegions.insert(AllArrays.GetArrays().begin(),
                              AllArrays.GetArrays().end());
    // Determine array distribution and alignment.
    createOptimalDistribution(G, ReducedG, AllArrays, APCRegion->GetId(),
                              false);
    compliteArrayUsage(AllArrays, CreatedArrays, FormalToActual, ArrayIds);
  }
  remoteNotUsedArrays(CreatedArrays, ArraysInAllRegions, FormalToActual);
  for (auto *APCRegion : Regions) {
    auto &G{APCRegion->GetGraphToModify()};
    auto &ReducedG{APCRegion->GetReducedGraphToModify()};
    auto &AllArrays{APCRegion->GetAllArraysToModify()};
    auto &DataDirs{APCRegion->GetDataDirToModify()};
    createDistributionDirs(ReducedG, AllArrays, DataDirs, APCMsgs,
                           FormalToActual, false);
    createAlignDirs(ReducedG, AllArrays, DataDirs, APCRegion->GetId(),
                    FormalToActual, APCMsgs);
    // Normalize alignment, make all templates start from zero.
    shiftAlignRulesForTemplates(AllArrays.GetArrays(), APCRegion->GetId(),
                                DataDirs, FormalToActual);
    std::vector<int> FullDistrVariant;
    FullDistrVariant.reserve(DataDirs.distrRules.size());
    for (auto TplInfo : DataDirs.distrRules)
      FullDistrVariant.push_back(TplInfo.second.size() - 1);
    APCRegion->SetCurrentVariant(std::move(FullDistrVariant));
    for (auto *A : AllArrays.GetArrays()) {
      if (A->IsTemplate()) {
        auto *Tpl{ParallelCtx.makeTemplate(A->GetShortName())};
        APCCtx.addArray(Tpl, A);
        auto APCSymbol{new apc::Symbol(&APCCtx, Tpl)};
        APCCtx.addSymbol(APCSymbol);
        A->SetDeclSymbol(APCSymbol);
      } else if (A->IsLoopArray()) {
        auto FileItr{Files.find(A->GetDeclInfo().begin()->first)};
        assert(FileItr != Files.end() && "File must be known!");
        auto I{FileItr->second.get<apc::LoopGraph>().find(
            A->GetDeclInfo().begin()->second)};
        assert(I != FileItr->second.get<apc::LoopGraph>().end() &&
               "A loop the array is attached to must be known!");
        auto APCSymbol{new apc::Symbol(&APCCtx, I->second->loop)};
        APCCtx.addSymbol(APCSymbol);
        A->SetDeclSymbol(APCSymbol);
      }
    }
    LLVM_DEBUG(dotGraphLog(*APCRegion));
    copy_if(AllArrays.GetArrays(),
            std::inserter(DistributedArrays, DistributedArrays.end()),
            [](auto *A) { return !A->IsNotDistribute(); });
  }
  LLVM_DEBUG(printDistribution(dbgs()));
}

void APCParallelizationPass:: selectComputationDistribution(
    const FormalToActualMap &FormalToActual, const NameToFunctionMap &Functions,
    FileInfoMap::value_type &FileInfo, apc::ParallelRegion &APCRegion,
    std::vector<apc::Directive *> &ParallelDirs, FileToMessageMap &APCMsgs) {
  auto &ReducedG{APCRegion.GetReducedGraphToModify()};
  auto &AllArrays{APCRegion.GetAllArraysToModify()};
  auto &DataDirs{APCRegion.GetDataDir()};
  std::vector<std::pair<apc::Array *, const DistrVariant *>> CurrentVariant;
  for (std::size_t I = 0, EI = APCRegion.GetCurrentVariant().size(); I < EI;
       ++I)
    CurrentVariant.push_back(std::make_pair(
        DataDirs.distrRules[I].first,
        &DataDirs.distrRules[I].second[APCRegion.GetCurrentVariant()[I]]));
  const std::map<apc::LoopGraph *, void *> DepInfoForLoopGraph;
  selectParallelDirectiveForVariant(
      nullptr, &APCRegion, ReducedG, AllArrays, FileInfo.second.get<Root>(),
      FileInfo.second.get<apc::LoopGraph>(), Functions, CurrentVariant,
      DataDirs.alignRules, ParallelDirs, APCRegion.GetId(), FormalToActual,
      DepInfoForLoopGraph,
      getObjectForFileFromMap(FileInfo.getKeyData(), APCMsgs));
  for (auto *Expr : FileInfo.second.get<apc::ArrayRefExp>()) {
    if (!Expr->isInLoop() || Expr->getArray()->IsNotDistribute())
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
      MaxNumberOfRecInDim =
          std::max<int>(MaxNumberOfRecInDim, Expr->getRecInDim(DimIdx).size());
    }
    checkArrayRefInLoopForRemoteStatus(
        MultipleRecInDim, NumberOfRec, Expr->size(), MaxNumberOfRecInDim, 0,
        Expr->getArray(), NumberOfDimsWithRec, Expr,
        FileInfo.second.get<LoopToArrayMap>(), DimWithRec,
        FileInfo.second.get<apc::LoopGraph>(), FormalToActual, &APCRegion,
        LoopNest);
  }
}

void APCParallelizationPass::updateParallelization(
    APCToIRLoopsMap &APCToIRLoops, const FormalToActualMap &FormalToActual,
    const FileInfoMap::value_type &FileInfo, const NameToFunctionMap &Functions,
    apc::ParallelRegion &APCRegion, DVMHParallelizationContext &ParallelCtx,
    std::vector<apc::Directive *> &ParallelDirs, FileToMessageMap &APCMsgs) {
  using DirectiveList = SmallVector<DirectiveImpl *, 16>;
  DenseMap<Function *,
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
  auto &ReducedG{APCRegion.GetReducedGraphToModify()};
  auto &AllArrays{APCRegion.GetAllArraysToModify()};
  auto &DataDirs{APCRegion.GetDataDir()};
  for (auto [F, DirInfo] : FuncToDirs) {
    auto &LpInfo{getAnalysis<LoopInfoWrapperPass>(*F).getLoopInfo()};
    for (auto &&[Anchor, DirList] : DirInfo) {
      auto *LpStmt{cast<apc::LoopStatement>(Anchor)};
      auto *HeaderBB{std::get<BasicBlock *>(DirList)};
      auto EntryInfo{ParallelCtx.getParallelization().try_emplace(HeaderBB)};
      assert(EntryInfo.second && "Unable to create a parallel block!");
      EntryInfo.first->get<ParallelLocation>().emplace_back();
      EntryInfo.first->get<ParallelLocation>().back().Anchor = LpStmt->getId();
      auto *LLVMLoop{LpInfo.getLoopFor(HeaderBB)};
      assert(LLVMLoop && "LLVM IR representation of a loop must be known!");
      auto ExitingBB{getValidExitingBlock(*LLVMLoop)};
      assert(ExitingBB && "Parallel loop must have a single exit!");
      ParallelLocation *ExitLoc{nullptr};
      if (ExitingBB == LLVMLoop->getHeader()) {
        ExitLoc = &EntryInfo.first->get<ParallelLocation>().back();
      } else {
        auto ExitInfo{ParallelCtx.getParallelization().try_emplace(ExitingBB)};
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
          assert(Dir->RawPragma && "Raw representation of a parallel directive "
                                   "must be available!");
          auto RemoteList{createRemoteInParallel(
              std::pair{LpStmt->getLoop(),
                        static_cast<apc::ParallelDirective *>(Dir->RawPragma)},
              AllArrays, FileInfo.second.get<LoopToArrayMap>(), ReducedG,
              DataDirs, APCRegion.GetCurrentVariant(),
              getObjectForFileFromMap(FileInfo.getKeyData(), APCMsgs),
              APCRegion.GetId(), FormalToActual, Functions)};
          for (auto &&[Str, Expr] : RemoteList) {
            auto S{Expr->getArray()->GetDeclSymbol()};
            dvmh::Align Align{S->getVariable(F).getValue(), Expr->size()};
            for (unsigned I = 0, EI = Expr->size(); I < EI; ++I) {
              if (Expr->isConstantSubscript(I))
                Align.Relation[I] = Expr->getConstantSubscript(I);
            }
            cast<PragmaParallel>(*Dir->Pragma)
                .getClauses()
                .get<Remote>()
                .insert(std::move(Align));
          }
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
        ExitLoc->Exit.push_back(std::make_unique<ParallelMarker<PragmaRegion>>(
            0, DVMHRegion.get()));
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

void APCParallelizationPass::addRemoteAccessDirectives(
    Function &ClientF, const std::set<apc::Array *> &DistributedArrays,
    APCContext &APCCtx, DVMHParallelizationContext &ParallelCtx) {
  auto *FI{APCCtx.findFunction(ClientF)};
  if (!FI)
    return;
  auto &TLIP{getAnalysis<TargetLibraryInfoWrapperPass>()};
  auto &TLI{TLIP.getTLI(ClientF)};
  auto &Provider{getAnalysis<APCDataDistributionProvider>(ClientF)};
  auto &ClientDIAT{Provider.get<DIEstimateMemoryPass>().getAliasTree()};
  AnalysisSocket *Socket{nullptr};
  if (auto *SI{getAnalysisIfAvailable<AnalysisSocketImmutableWrapper>()})
    Socket = (*SI)->getActiveSocket();
  DIMemoryClientServerInfo ClientServerInfo(ClientDIAT, Socket, ClientF);
  assert(ClientServerInfo.isValidMemory() &&
         "Analysis information must be available!");
  SmallPtrSet<const DIAliasNode *, 8> DistinctMemory;
  for (auto &DIM : make_range(ClientServerInfo.DIAT->memory_begin(),
                              ClientServerInfo.DIAT->memory_end()))
    if (auto *DIUM{dyn_cast<DIUnknownMemory>(&DIM)}; DIUM && DIUM->isDistinct())
      DistinctMemory.insert(DIUM->getAliasNode());
  SpanningTreeRelation<const DIAliasTree *> STR{ClientServerInfo.DIAT};
  DenseMap<const DIAliasNode *, SmallVector<dvmh::Align, 1>>
      ToDistribute;
  for (auto *A : DistributedArrays) {
    if (A->IsTemplate() || A->IsLoopArray())
      continue;
    auto *S{A->GetDeclSymbol()};
    auto Var{S->getVariable(&ClientF)};
    if (!Var)
      continue;
    auto DIMI{ClientServerInfo.getMemory(&*Var->get<MD>())};
    auto I{ToDistribute.try_emplace(DIMI->getAliasNode()).first};
    if (auto Itr{find_if(I->second,
                         [&Var](const dvmh::Align &A) {
                           return std::get<VariableT>(A.Target) == *Var;
                         })};
        Itr == I->second.end())
      I->second.emplace_back(std::move(*Var), A->GetDimSize());
  }
  auto &LI{Provider.get<LoopInfoWrapperPass>().getLoopInfo()};
  Optional<SmallVector<dvmh::Align, 8>> ToAccessDistinct;
  for (auto &I : instructions(&ClientF)) {
    // Do not attach remote_access to calls. It has to be inserted in the body
    // of callee. If body is not available we cannot distribute an accessed
    // array. This check must be performed earlier.
    if (isa<CallBase>(I))
      continue;
    if (isInParallelNest(I, LI, ParallelCtx.getParallelization()).first)
      continue;
    SmallVector<dvmh::Align, 8> ToAccess;
    auto addToAccessIfNeed = [&STR, &ToDistribute](auto *CurrentAN,
                                                   auto &ToAccess) {
      if (auto Itr{find_if(ToDistribute,
                           [&STR, CurrentAN](auto &Data) {
                             return !STR.isUnreachable(Data.first, CurrentAN);
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
          auto CToSDIM{ClientServerInfo.findFromClient(*EM->getTopLevelParent(),
                                                       DL, DT)};
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
    if (!ToAccess.empty() || IsDistinctAccessed && !ToAccessDistinct->empty()) {
      auto RemoteRef{
          ParallelCtx.getParallelization().emplace<PragmaRemoteAccess>(
              I.getParent(), &I, true /*OnEntry*/, false /*IsRequired*/,
              false /*IsFinal*/)};
      cast<PragmaRemoteAccess>(RemoteRef)->getMemory().insert(ToAccess.begin(),
                                                              ToAccess.end());
      if (IsDistinctAccessed)
        cast<PragmaRemoteAccess>(RemoteRef)->getMemory().insert(
            ToAccessDistinct->begin(), ToAccessDistinct->end());
      ParallelCtx.getParallelization()
          .emplace<ParallelMarker<PragmaRemoteAccess>>(
              I.getParent(), &I, false /*OnEntry*/, 0,
              cast<PragmaRemoteAccess>(RemoteRef));
    }
  }
}

bool APCParallelizationPass::runOnModule(Module &M) {
  auto *DIArrayInfo{getAnalysis<DIArrayAccessWrapper>().getAccessInfo()};
  if (!DIArrayInfo) {
    //TODO (kaniandr@gmail.com): emit warning
    return false;
  }
  auto &GlobalOpts{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  APCDataDistributionProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&GlobalOpts](GlobalOptionsImmutableWrapper &GO) {
        GO.setOptions(&GlobalOpts);
      });
  auto &MemEnv{getAnalysis<DIMemoryEnvironmentWrapper>().get()};
  APCDataDistributionProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&MemEnv](DIMemoryEnvironmentWrapper &Env) { Env.set(MemEnv); });
  auto &Globals{getAnalysis<GlobalsAccessWrapper>().get()};
  APCDataDistributionProvider::initialize<GlobalsAccessWrapper>(
      [&Globals](GlobalsAccessWrapper &Wrapper) { Wrapper.set(Globals); });
  auto &APCCtx{getAnalysis<APCContextWrapper>().get()};
  RegionList Regions{&APCCtx.getDefaultRegion()};
  for (auto *R : Regions)
    if (!R->GetDataDir().distrRules.empty())
      return false;
  // TODO (kaniandr@gmail.com): what should we do if an array is a function
  // parameter, however it is not accessed in a function. In this case,
  // this array is not processed by this pass and it will not be added in
  // a graph of arrays. So, there is no information that this array should
  // be mentioned in #pragma dvm inherit(...) directive.
  // It is not possible to delinearize this array without interprocedural
  // analysis, so it has not been stored in the list of apc::Array.
  // At this moment the inability to determine correspondence between actual
  // and formal parameters for this array prevents this array distribution.
  ArrayToKeyMap ArrayIds;
  for (auto &&[Id, A] : APCCtx.mImpl->Arrays) {
    auto &DeclInfo{*A->GetDeclInfo().begin()};
    PFIKeyT PFIKey{DeclInfo.second, DeclInfo.first, A->GetShortName()};
    ArrayIds.try_emplace(A.get(), PFIKey);
  }
  NameToFunctionMap Functions;
  FileToFuncMap FileToFunc;
  FileToLoopMap FileToLoop;
  FuncToLoopMap FuncToLoop;
  APCToIRLoopsMap APCToIRLoops;
  FileInfoMap Files;
  ArrayAccessPool AccessPool;
  ArrayAccessSummary ArrayRWs;
  collectFunctionsAndLoops(M, APCCtx, Functions, FileToFunc, Files, FileToLoop,
                           FuncToLoop, APCToIRLoops);
  // Initial initialization of loop properties that depends on arrays accesses
  // to be collected further.
  for (auto &&[F, LoopList] : FileToLoop)
    for (auto *APCLoop : LoopList) {
      APCLoop->hasUnknownArrayAssigns = false;
      APCLoop->hasIndirectAccess = false;
      APCLoop->hasUnknownDistributedMap = false;
      APCLoop->withoutDistributedArrays = true;
    }
  for (auto &Access : *DIArrayInfo) {
    auto *APCArray{APCCtx.findArray(Access.getArray()->getAsMDNode())};
    if (!APCArray)
      continue;
    ArrayRWs.emplace(ArrayIds[APCArray], std::make_pair(APCArray, nullptr));
  }
  std::set<apc::Array *> DistributedArrays;
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  try {
    FormalToActualMap FormalToActual;
    checkCountOfIter(FileToLoop, FileToFunc, APCCtx.mImpl->Diags);
    bindFormalAndActualPrameters(ArrayRWs, FileToFunc, FormalToActual,
                                 APCCtx.mImpl->Diags);
    for (auto &Access : *DIArrayInfo)
      collectArrayAccessInfo(M, Access, APCCtx, AccessPool, Files, ArrayIds);
    for (auto &Info : Files) {
      auto &Msgs{getObjectForFileFromMap(Info.getKey().str().c_str(),
                                         APCCtx.mImpl->Diags)};
      for (auto &&[Loc, L] : Info.second.get<apc::LoopGraph>())
        L->addConflictMessages(&Msgs);
    }
    // A stub variable which is not used at this moment.
    std::map<PFIKeyT, apc::Array *> CreatedArrays;
    buildDataDistributionGraph(Regions, FormalToActual, ArrayRWs, FileToFunc,
                               FuncToLoop, FileToLoop, Files, CreatedArrays,
                               APCCtx.mImpl->Diags);
    buildDataDistribution(Regions, FormalToActual, ArrayIds, Files, APCCtx,
                          ParallelCtx, DistributedArrays, CreatedArrays,
                          APCCtx.mImpl->Diags);
    for (auto &FileInfo : Files) {
      createParallelDirectives(
          FileInfo.second.get<LoopToArrayMap>(), Regions, FormalToActual,
          getObjectForFileFromMap(FileInfo.getKeyData(), APCCtx.mImpl->Diags));
      if (auto FuncItr{FileToFunc.find(FileInfo.getKey().str())};
          FuncItr != FileToFunc.end())
        for (auto *FI : FuncItr->second)
          if (auto LoopItr{FuncToLoop.find(FI)}; LoopItr != FuncToLoop.end())
            selectFreeLoopsForParallelization(
                LoopItr->get<Root>(), FI->funcName, false, Regions,
                getObjectForFileFromMap(FileInfo.getKeyData(),
                                        APCCtx.mImpl->Diags));
      UniteNestedDirectives(FileInfo.second.get<Root>());
      for (auto *APCRegion : Regions) {
        std::vector<apc::Directive *> ParallelDirs;
        selectComputationDistribution(FormalToActual, Functions, FileInfo,
                                      *APCRegion, ParallelDirs,
                                      APCCtx.mImpl->Diags);
        updateParallelization(APCToIRLoops, FormalToActual, FileInfo, Functions,
                              *APCRegion, ParallelCtx, ParallelDirs,
                              APCCtx.mImpl->Diags);
      }
    }
    createShadowSpec(FileToLoop, FormalToActual, DistributedArrays);
  } catch (...) {
    LLVM_DEBUG(dbgs() << getGlobalBuffer());
    return false;
  }
  for (auto &F : M)
    addRemoteAccessDirectives(F, DistributedArrays, APCCtx, ParallelCtx);
  return false;
}

static void collectAccesses(
    LoopGraph *L,
    DenseMap<apc::Array *, std::vector<std::vector<std::pair<int, int>>>>
        &Accesses) {
  for (auto *A : cast<apc::LoopStatement>(L->loop)->getImmediateAccesses()) {
    auto &CurrentArray{Accesses[A->getArray()]};
    CurrentArray.emplace_back();
    auto &CurrentAccess{CurrentArray.back()};
    using value_type = std::decay_t<decltype(CurrentAccess)>::value_type;
    LLVM_DEBUG(dbgs() << "[APC]: collect affine accesses to "
                      << A->getArray()->GetShortName() << " (" << A->getArray()
                      << ")\n");
    CurrentAccess.resize(
        A->getArray()->GetDimSize(),
        std::pair{std::numeric_limits<value_type::first_type>::min(),
                  std::numeric_limits<value_type::second_type>::max()});
    for (unsigned I = 0, EI = A->size(); I < EI; ++I) {
      if (A->isAffineSubscript(I))
        if (auto S{A->getAffineSubscript(I)};
            S.size() == 1 &&
            S.getConstant().getActiveBits() <=
                sizeof(value_type::second_type) * CHAR_BIT &&
            S.getMonom(0).Value.getActiveBits() <=
                sizeof(value_type::first_type) * CHAR_BIT) {
          CurrentAccess[I] = value_type{S.getMonom(0).Value.getSExtValue(),
                                        S.getConstant().getSExtValue()};
          LLVM_DEBUG(dbgs() << "[APC]: collect affine access in a loop nest "
                            << S.getMonom(0).Value << " * I + "
                            << S.getConstant() << "\n");
        }
    }
  }
  for (auto *Child : L->children)
    collectAccesses(Child, Accesses);
}

void APCParallelizationPass::printDistribution(raw_ostream &OS) const {
  auto &APCCtx{getAnalysis<APCContextWrapper>().get()};
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
        return LpStmt->hasInduction() &&
               LpStmt->getInduction().template get<AST>()->getName() == Name;
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
  auto APCSymbol{MapTo->GetDeclSymbol()};
  assert(APCSymbol && "Unknown array symbol!");
  if (APCSymbol->isTemplate())
    Clauses.get<dvmh::Align>() = dvmh::Align{APCSymbol->getTemplate()};
  else {
    Clauses.get<dvmh::Align>() =
        dvmh::Align{APCSymbol->getVariable(Func).getValue()};
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
      assert(!A->IsLoopArray() &&
             "Loop cannot be referenced in across clause!");
      auto AcrossItr{
          Clauses.get<trait::Dependence>()
              .try_emplace(A->GetDeclSymbol()->getVariable(Func).getValue())
              .first};
      AcrossItr->second.second = false;
      for (unsigned Dim = 0, DimE = across[I].second.size(); Dim < DimE;
           ++Dim) {
        auto Left{across[I].second[Dim].first + acrossShifts[I][Dim].first};
        auto Right{across[I].second[Dim].second + acrossShifts[I][Dim].second};
        AcrossItr->second.get<trait::DIDependence::DistanceVector>()
            .emplace_back(APSInt{APInt{64, (uint64_t)Left, false}, true},
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
    LLVM_DEBUG(dbgs() << "[APC]: collect affine accesses in the loop nest at "
                      << CurrLoop->lineNum << "\n");
    DenseMap<apc::Array *, std::vector<std::vector<std::pair<int, int>>>>
        Accesses;
    collectAccesses(CurrLoop, Accesses);
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
      assert(!A->IsLoopArray() &&
             "Loop cannot be referenced in shadow_renew clause!");
      LLVM_DEBUG(dbgs() << "[APC]: calculate shadow_renew for "
                        << A->GetShortName() << " (" << A << ")\n");
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
        ShadowItr->second.get<trait::DIDependence::DistanceVector>()
            .emplace_back(APSInt{APInt{64, (uint64_t)Left, false}, true},
                          APSInt{APInt{64, (uint64_t)Right, false}, true});
      }
      ShadowItr->second.get<Corner>() =
          (shadowRenew[I].second.size() > 1 &&
           needCorner(A, ShiftsByAccess, Accesses[A]));
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
    for (auto Range : Distance.first) {
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

void addRemoteLink(const apc::LoopGraph *Loop,
                   const std::map<std ::string, apc::FuncInfo *> &Functions,
                   apc::ArrayRefExp *Expr,
                   std::map<std::string, apc::ArrayRefExp *> &UniqueRemotes,
                   const std::set<std::string> &RemotesInParallel,
                   std::set<ArrayRefExp *> &AddedRemotes,
                   const std::vector<std::string>& MapToLoop,
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
