//===- DVMHDataTransferIPO.cpp - Actual/Get Actual IPO -----------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// This file implements IPO of data transfer between CPU and GPU memories.
//
//===----------------------------------------------------------------------===//

#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/AnalysisSocket.h"
#include "tsar/Analysis/Attributes.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/GlobalsAccess.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Analysis/Clang/RegionDirectiveInfo.h"
#include "tsar/Transform/Clang/DVMHDirecitves.h"
#include "tsar/Transform/Clang/Passes.h"
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/MapVector.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/InitializePasses.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Operator.h>
#include <llvm/Pass.h>

using namespace llvm;
using namespace tsar;
using namespace tsar::dvmh;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-ipo"

namespace {
using DVMHDataTransferIPOPassProvider = FunctionPassAAProvider<
    LoopInfoWrapperPass, DominatorTreeWrapperPass, PostDominatorTreeWrapperPass,
    EstimateMemoryPass, DIEstimateMemoryPass, GlobalOptionsImmutableWrapper>;

class DVMHDataTransferIPOPass : public ModulePass, private bcl::Uncopyable {
  /// Stack of lists which contain actual/get_actual directives.
  /// If optimization is successful for a level then the corresponding
  /// directives can be removed.
  ///
  /// The 'Hierarchy' field contains directives which have to be inserted
  /// instead of optimized ones from the 'Sibling' field.
  using RegionDataReplacement = std::vector<bcl::tagged_pair<
      bcl::tagged<SmallVector<ParallelLevel *, 4>, Sibling>,
      bcl::tagged<SmallVector<ParallelItem *, 4>, Hierarchy>>>;

  /// Variables which are presented in some actual or get_actual directive
  /// correspondingly. The key is an alias node which contains a corresponding
  /// memory on the analysis server. The 'Hierarchy' field identifies the
  /// innermost level (in the RegionDataReplacement stack) which contains a
  /// corresponding directive. So, the levels under the 'Hierarchy' level does
  /// not depend on a corresponding memory locations. It means that
  /// optimization for the bellow level is possible even if some
  /// actual/get_actual directives which accesses mentioned memory locations
  /// cannot be inserted.
  using RegionDataCache = DenseMap<
      const DIAliasNode *, std::tuple<SmallVector<VariableT, 1>, unsigned>,
      DenseMapInfo<const DIAliasNode *>,
      TaggedDenseMapTuple<bcl::tagged<const DIAliasNode *, DIAliasNode>,
                          bcl::tagged<SmallVector<VariableT, 1>, VariableT>,
                          bcl::tagged<unsigned, Hierarchy>>>;

  using IPOMap = PersistentMap<
      const Function *,
      std::tuple<bool, SmallPtrSet<const Value *, 4>,
                 SmallVector<PragmaActual *, 4>,
                 SmallVector<PragmaGetActual *, 4>>,
      DenseMapInfo<const Function *>,
      TaggedDenseMapTuple<
          bcl::tagged<const Function *, Function>, bcl::tagged<bool, bool>,
          bcl::tagged<SmallPtrSet<const Value *, 4>, Value>,
          bcl::tagged<SmallVector<PragmaActual *, 4>, PragmaActual>,
          bcl::tagged<SmallVector<PragmaGetActual *, 4>, PragmaGetActual>>>;

public:
  static char ID;

  DVMHDataTransferIPOPass() : ModulePass(ID) {
    initializeDVMHDataTransferIPOPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<AnalysisSocketImmutableWrapper>();
    AU.addRequired<CallGraphWrapperPass>();
    AU.addRequired<ClangRegionCollector>();
    AU.addRequired<DIMemoryEnvironmentWrapper>();
    AU.addRequired<DVMHDataTransferIPOPassProvider>();
    AU.addRequired<DVMHParallelizationContext>();
    AU.addRequired<GlobalsAAWrapperPass>();
    AU.addRequired<GlobalsAccessWrapper>();
    AU.addRequired<GlobalOptionsImmutableWrapper>();
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.setPreservesAll();
  }

  void releaseMemory() override {
    mEntryPoint = nullptr;
    mSocket = nullptr;
    mFunctions.clear();
    mRegions.clear();
    mToActual.clear();
    mToGetActual.clear();
    mDistinctMemory.clear();
    mIPOToActual.clear();
    mIPOToGetActual.clear();
    mIPOMap.clear();
  }

private:
  std::pair<bool, bool> needToOptimize(const Function &F) const {
    if (mRegions.empty())
      return std::pair{true, true};
    bool Optimize{false}, OptimizeChildren{false};
    for (const auto *R : mRegions) {
      switch (R->contain(F)) {
      case OptimizationRegion::CS_No:
        continue;
      case OptimizationRegion::CS_Always:
      case OptimizationRegion::CS_Condition:
        Optimize = true;
        [[fallthrough]];
      case OptimizationRegion::CS_Child:
        OptimizeChildren = true;
        break;
      default:
        llvm_unreachable("Unkonwn region contain status!");
      }
      if (Optimize)
        break;
    }
    return std::pair{Optimize, OptimizeChildren};
  }

  bool needToOptimize(const Loop &L) const {
    if (mRegions.empty())
      return true;
    return any_of(mRegions, [&L](auto *R) { return R->contain(L); });
  }

  /// Initialize interprocedural optimization.
  ///
  /// \return `false` if optimization has to be omitted.
  bool initializeIPO(Module &M);

  bool optimizeUpward(Loop &L, const DVMHDataTransferIPOPassProvider &Provider) {
    return optimizeUpward(&L, L.begin(), L.end(), Provider);
  }

  template<class ItrT>
  bool optimizeUpward(PointerUnion<Loop *, Function *> Parent,
      ItrT I, ItrT EI, const DVMHDataTransferIPOPassProvider &Provider) {
    // We treat skipped levels as optimized ones.
    if (!optimizeGlobalIn(Parent, Provider))
      return true;
    bool Optimize{true};
    for (; I != EI; ++I)
      Optimize &= optimizeUpward(**I, Provider);
    if (Optimize)
      Optimize = optimizeGlobalOut(Parent, Provider);
    return Optimize;
  }

  /// Prepare level to upward optimization.
  ///
  /// Before the upward optimization levels in a function a traversed downward
  /// to reach innermost levels. So, this function is called when a level
  /// is visited for the first time.
  ///
  /// Return true if it is necessary to optimize this level and traverse
  /// inner levels.
  bool optimizeGlobalIn(PointerUnion<Loop *, Function *> Level,
    const DVMHDataTransferIPOPassProvider &Provider);

  /// Visit level in upward direction and perform optimization. Return `true`
  /// if optimization was successful.
  ///
  /// If a level has not been optimized, the outer levels will not be also
  /// optimized.
  bool optimizeGlobalOut(PointerUnion<Loop *, Function *> Level,
    const DVMHDataTransferIPOPassProvider &Provider);

  Function *mEntryPoint{nullptr};
  AnalysisSocket *mSocket{nullptr};
  MapVector<Function *, std::tuple<unsigned, bool>> mFunctions;
  SmallVector<const tsar::OptimizationRegion *, 4> mRegions;

  // Data structures for intraprocedural optimization
  RegionDataCache mToActual, mToGetActual;
  RegionDataReplacement mReplacementFor;
  SmallPtrSet<const DIAliasNode *, 8> mDistinctMemory;

  // Data structures for interprocedural optimization.
  DenseMap<const Value *, clang::VarDecl *> mIPOToActual, mIPOToGetActual;
  IPOMap mIPOMap;
};
}

bool DVMHDataTransferIPOPass::initializeIPO(Module &M) {
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  auto &GO{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  auto addToList = [this, &DL = M.getDataLayout()](auto &Memory,
                                                   auto &ToOptimize) {
    for (auto &Align : Memory) {
      auto &Var{std::get<VariableT>(Align.Target)};
      if (auto *DIEM{
              dyn_cast_or_null<DIEstimateMemory>(Var.template get<MD>())};
          DIEM && !DIEM->emptyBinding() && *DIEM->begin() &&
          isa<GlobalVariable>(getUnderlyingObject(*DIEM->begin(), 0))) {
        assert(DIEM->begin() + 1 == DIEM->end() &&
               "Alias tree is corrupted: multiple binded globals!");
        ToOptimize.try_emplace(*DIEM->begin(), Var.template get<AST>());
      }
    }
  };
  for (auto &PLocList : ParallelCtx.getParallelization()) {
    auto &LI{getAnalysis<LoopInfoWrapperPass>(
                 *PLocList.get<BasicBlock>()->getParent())
                 .getLoopInfo()};
    if (auto *L{LI.getLoopFor(PLocList.get<BasicBlock>())}) {
      if (!needToOptimize(*L))
        continue;
    } else if (!needToOptimize(*PLocList.get<BasicBlock>()->getParent())
                    .first) {
      continue;
    }
    for (auto &PLoc : PLocList.get<ParallelLocation>()) {
      for (auto &PI : PLoc.Entry)
        if (auto *Actual{dyn_cast<PragmaActual>(PI.get())})
          addToList(Actual->getMemory(), mIPOToActual);
        else if (auto *GetActual{dyn_cast<PragmaGetActual>(PI.get())})
          addToList(GetActual->getMemory(), mIPOToGetActual);
      for (auto &PI : PLoc.Exit)
        if (auto *Actual{dyn_cast<PragmaActual>(PI.get())})
          addToList(Actual->getMemory(), mIPOToActual);
        else if (auto *GetActual{dyn_cast<PragmaGetActual>(PI.get())})
          addToList(GetActual->getMemory(), mIPOToGetActual);
    }
  }
  auto &CG{getAnalysis<CallGraphWrapperPass>().getCallGraph()};
  SmallPtrSet<Function *, 16> ExternalCalls;
  if (CG.getExternalCallingNode())
    for (auto &ExternalCall : *CG.getExternalCallingNode())
      if (auto *F{ExternalCall.second->getFunction()})
        ExternalCalls.insert(F);
  auto &SocketInfo{getAnalysis<AnalysisSocketImmutableWrapper>()};
  auto &Socket{SocketInfo->getActive()->second};
  auto RM{Socket.getAnalysis<AnalysisClientServerMatcherWrapper>()};
  auto &ClientToServer{**RM->value<AnalysisClientServerMatcherWrapper *>()};
  bool InRecursion{false};
  auto Range{reverse(mFunctions)};
  for (auto FuncItr{Range.begin()}, FuncItrE{Range.end()}; FuncItr != FuncItrE;
       ++FuncItr) {
    auto *F{FuncItr->first};
    auto [SCCId, InCycle] = FuncItr->second;
    if (F->isDeclaration()) {
      mIPOToActual.clear();
      mIPOToGetActual.clear();
      ParallelCtx.getIPORoot().invalidate();
      continue;
    }
    auto [OptimizeAll, OptimizeChildren] = needToOptimize(*F);
    if (!OptimizeChildren)
      continue;
    if (ParallelCtx.isParallelCallee(*F))
      continue;
    IPOMap::persistent_iterator ToIgnoreItr;
    bool IsNew;
    std::tie(ToIgnoreItr, IsNew) = mIPOMap.try_emplace(F);
    if (!GO.NoExternalCalls && ExternalCalls.count(F) || InCycle ||
        !OptimizeAll || any_of(F->users(), [](auto *U) {
          return !isa<CallBase>(U) &&
                 (!isa<BitCastOperator>(U) || any_of(U->users(), [](auto *U) {
                   return !isa<CallBase>(U);
                 }));
        }))
      ToIgnoreItr->get<bool>() = false;
    else if (IsNew)
      ToIgnoreItr->get<bool>() = true;
    if (!InRecursion && InCycle) {
      auto StashItr{FuncItr};
      for (;
           FuncItr != FuncItrE && SCCId == std::get<unsigned>(FuncItr->second);
           ++FuncItr)
        for (auto &I : instructions(*std::get<Function *>(*FuncItr)))
          for (auto &Op : I.operands()) {
            auto Ptr{getUnderlyingObject(Op.get(), 0)};
            if (!isa<GlobalVariable>(Ptr) ||
                (!mIPOToActual.count(Ptr) && !mIPOToGetActual.count(Ptr)))
              continue;
            if (auto *Call{dyn_cast<CallBase>(&I)};
                Call && Call->isArgOperand(&Op))
              if (auto Callee{dyn_cast<Function>(
                      Call->getCalledOperand()->stripPointerCasts())}) {
                if (auto SCCInfoItr{mFunctions.find(Callee)};
                    SCCInfoItr == mFunctions.end() ||
                    std::get<unsigned>(SCCInfoItr->second) != SCCId)
                  continue;
                Callee = &cast<Function>(*ClientToServer[Callee]);
                if (Callee->arg_size() > Call->getArgOperandNo(&Op) &&
                    Callee->getArg(Call->getArgOperandNo(&Op))
                        ->hasAttribute(Attribute::NoCapture))
                  ToIgnoreItr->get<Value>().insert(Ptr);
              }
          }
      FuncItr = StashItr;
      for (;
           FuncItr != FuncItrE && SCCId == std::get<unsigned>(FuncItr->second);
           ++FuncItr) {
        auto IPOInfoItr{
            mIPOMap.try_emplace(std::get<Function *>(*FuncItr)).first};
        IPOInfoItr->get<Value>() = ToIgnoreItr->get<Value>();
      }
      FuncItr = StashItr;
      InRecursion = true;
    } else if (InRecursion && !InCycle) {
      InRecursion = false;
    }
    for (auto &I : instructions(F)) {
      if (isa<LoadInst>(I))
        continue;
      for (auto &Op: I.operands()) {
        auto Ptr{getUnderlyingObject(Op.get(), 0)};
        if (!isa<GlobalVariable>(Ptr) ||
            (!mIPOToActual.count(Ptr) && !mIPOToGetActual.count(Ptr)))
          continue;
        if (auto *SI{dyn_cast<StoreInst>(&I)}) {
          if (SI->getValueOperand() == Op) {
            mIPOToActual.erase(Ptr);
            mIPOToGetActual.erase(Ptr);
          }
        } else if (auto *Call{dyn_cast<CallBase>(&I)}) {
            if (Call->isArgOperand(&Op)) {
              if (auto Callee{dyn_cast<Function>(
                      Call->getCalledOperand()->stripPointerCasts())}) {
                Callee = &cast<Function>(*ClientToServer[Callee]);
                if (Callee->arg_size() > Call->getArgOperandNo(&Op) &&
                    Callee->getArg(Call->getArgOperandNo(&Op))
                         ->hasAttribute(Attribute::NoCapture)) {
                  if (!Callee->isDeclaration()) {
                    auto [I, IsNew] = mIPOMap.try_emplace(Callee);
                    if (IsNew)
                      I->get<Value>() = ToIgnoreItr->get<Value>();
                    I->get<Value>().insert(Ptr);
                  }
                  continue;
                }
              }
            }
          mIPOToActual.erase(Ptr);
          mIPOToGetActual.erase(Ptr);
        } else if (!isa<BitCastInst>(I) && !isa<GetElementPtrInst>(I)) {
          mIPOToActual.erase(Ptr);
          mIPOToGetActual.erase(Ptr);
        }
      }
    }
  }
  LLVM_DEBUG(
    dbgs() << "[DVMH IPO]: enable IPO for: ";
    for (auto &&[F, Node] : mFunctions)
      if (auto I{mIPOMap.find(F)}; I != mIPOMap.end() && I->get<bool>()) {
        dbgs() << F->getName() << " ";
        if (!I->get<Value>().empty()) {
          dbgs() << "(exclude:";
          for (auto *V : I->get<Value>()) {
            if (auto VarItr{mIPOToActual.find(V)}; VarItr != mIPOToActual.end())
              dbgs() << " " << VarItr->second->getName();
            else if (auto VarItr{mIPOToGetActual.find(V)};
                     VarItr != mIPOToGetActual.end())
              dbgs() << " " << VarItr->second->getName();
          }
          dbgs() << ") ";
        }
      }
    dbgs() << "\n";
    dbgs() << "[DVMH IPO]: disable IPO for: ";
    for (auto &&[F, Node] : mFunctions)
      if (auto I{mIPOMap.find(F)}; I == mIPOMap.end() || !I->get<bool>()) {
        dbgs() << F->getName();
        if (I != mIPOMap.end())
          dbgs() << "(skip)";
        dbgs() << " ";
      }
    dbgs() << "\n";
    dbgs() << "[DVMH IPO]: IPO, optimize accesses to ";
    dbgs() << "actual: ";
    for (auto [V, D] : mIPOToActual)
      dbgs() << D->getName() << " ";
    dbgs() << "get_actual: ";
    for (auto [V, D] : mIPOToGetActual)
      dbgs() << D->getName() << " ";
    dbgs() << "\n";
  );
  return true;
}

bool DVMHDataTransferIPOPass::optimizeGlobalIn(
    PointerUnion<Loop *, Function *> Level,
    const DVMHDataTransferIPOPassProvider &Provider) {
  LLVM_DEBUG(
      dbgs() << "[DVMH IPO]: global optimization, visit level downward\n");
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  if (Level.is<Loop *>())
    if (isParallel(Level.get<Loop *>(), ParallelCtx.getParallelization()))
      return false;
  auto &F{Level.is<Function *>()
              ? *Level.get<Function *>()
              : *Level.get<Loop *>()->getHeader()->getParent()};
  auto [OptimizeAll, OptimizeChildren] = needToOptimize(F);
  auto IPOInfoItr{mIPOMap.find(&F)};
  if (IPOInfoItr == mIPOMap.end() && OptimizeChildren)
    return false;
  auto RM{mSocket->getAnalysis<AnalysisClientServerMatcherWrapper,
                              ClonedDIMemoryMatcherWrapper>()};
  auto &ClientToServer{**RM->value<AnalysisClientServerMatcherWrapper *>()};
  auto &ServerF{cast<Function>(*ClientToServer[&F])};
  auto &CSMemoryMatcher{
      *(**RM->value<ClonedDIMemoryMatcherWrapper *>())[ServerF]};
  auto &LI{Provider.get<LoopInfoWrapperPass>().getLoopInfo()};
  auto findDIM = [&F, &Provider](const Value *V, auto *D) -> VariableT {
    assert(V && "Value must not be null!");
    auto &AT{Provider.get<EstimateMemoryPass>().getAliasTree()};
    auto &DIAT{Provider.get<DIEstimateMemoryPass>().getAliasTree()};
    auto &DT{Provider.get<DominatorTreeWrapperPass>().getDomTree()};
    const auto &DL{F.getParent()->getDataLayout()};
    auto *EM{AT.find(MemoryLocation(V, LocationSize::precise(0)))};
    VariableT Var;
    Var.get<MD>() = nullptr;
    Var.get<AST>() = D;
    if (!EM)
      return Var;
    auto RawDIM{getRawDIMemoryIfExists(*EM->getTopLevelParent(), F.getContext(),
                                       DL, DT)};
    assert(RawDIM && "Accessed variable must be presented in alias tree!");
    auto DIMOriginItr{DIAT.find(*RawDIM)};
    assert(DIMOriginItr != DIAT.memory_end() &&
           "Existing memory must be presented in metadata alias tree.");
    Var.get<MD>() = &*DIMOriginItr;
    return Var;
  };
  auto collectInCallee = [this, &ParallelCtx, &CSMemoryMatcher,
                          &findDIM](Instruction *Call, Function *Callee,
                                    bool IsFinal) {
    auto IPOInfoItr{mIPOMap.find(Callee)};
    if (IPOInfoItr == mIPOMap.end() || !IPOInfoItr->get<bool>())
      return;
    auto sortMemory = [this, &CSMemoryMatcher, &findDIM](
                          auto &FromList, auto &LocalToOptimize, bool IsFinal,
                          SmallVectorImpl<VariableT> &FinalMemory,
                          SmallVectorImpl<VariableT> &ToOptimizeMemory) {
      for (PragmaData *PI : FromList)
        for (auto &Align : PI->getMemory()) {
          auto &Var{std::get<VariableT>(Align.Target)};
          assert(isa<DIGlobalVariable>(
                     cast<DIEstimateMemory>(Var.get<MD>())->getVariable()) &&
                 "IPO is now implemented for global variables only!");
          auto CallerVar{findDIM(&**Var.get<MD>()->begin(), Var.get<AST>())};
          assert(CallerVar.get<MD>() &&
                 "Metadata-level memory location must not be null!");
          if (!IsFinal) {
            auto DIMI{CSMemoryMatcher.find<Origin>(&*CallerVar.get<MD>())};
            auto I{LocalToOptimize.find(DIMI->get<Clone>()->getAliasNode())};
            if (I != LocalToOptimize.end()) {
              I->template get<Hierarchy>() = mReplacementFor.size() - 1;
              ToOptimizeMemory.push_back(std::move(CallerVar));
              continue;
            }
          }
          FinalMemory.push_back(std::move(CallerVar));
        }
    };
    auto addPragma = [this, &ParallelCtx,
                      Call](auto &&Type, bool OnEntry, bool IsFinal,
                            ArrayRef<VariableT> Memory) -> PragmaData * {
      auto Ref{ParallelCtx.getParallelization()
                   .emplace<std::decay_t<decltype(Type)>>(
          Call->getParent(), Call, OnEntry /*OnEntry*/, false /*IsRequired*/,
          IsFinal /*IsFinal*/)};
      for (auto &Var : Memory)
        cast<PragmaData>(Ref)->getMemory().emplace(std::move(Var));
      ParallelCtx.getIPORoot().child_insert(Ref.getUnchecked());
      Ref.getUnchecked()->parent_insert(&ParallelCtx.getIPORoot());
      return cast<PragmaData>(Ref);
    };
    if (!IPOInfoItr->get<PragmaActual>().empty()) {
      SmallVector<VariableT, 4> FinalMemory, ToOptimizeMemory;
      sortMemory(IPOInfoItr->get<PragmaActual>(), mToActual, IsFinal,
                 FinalMemory, ToOptimizeMemory);
      if (!FinalMemory.empty())
        addPragma(*IPOInfoItr->get<PragmaActual>().front(), true, true,
                  FinalMemory);
      if (!ToOptimizeMemory.empty())
        for (auto &Var : ToOptimizeMemory) {
          mReplacementFor.back().get<Sibling>().push_back(
              addPragma(*IPOInfoItr->get<PragmaActual>().front(), true, false,
                        makeArrayRef(Var)));
        }
    }
    if (!IPOInfoItr->get<PragmaGetActual>().empty()) {
      SmallVector<VariableT, 4> FinalMemory, ToOptimizeMemory;
      sortMemory(IPOInfoItr->get<PragmaActual>(), mToGetActual, IsFinal,
                 FinalMemory, ToOptimizeMemory);
      if (!FinalMemory.empty())
        addPragma(*IPOInfoItr->get<PragmaGetActual>().front(), false, true,
                  FinalMemory);
      if (!ToOptimizeMemory.empty())
        for (auto &Var : ToOptimizeMemory)
          mReplacementFor.back().get<Sibling>().push_back(
              addPragma(*IPOInfoItr->get<PragmaGetActual>().front(), false,
                        false, makeArrayRef(Var)));
    }
  };
  auto collectInCallees = [&collectInCallee](BasicBlock &BB, bool IsFinal) {
    for (auto &I : BB)
      if (auto *Call{dyn_cast<CallBase>(&I)})
        if (auto Callee{dyn_cast<Function>(
                Call->getCalledOperand()->stripPointerCasts())})
          collectInCallee(Call, Callee, IsFinal);
  };
  assert(!Level.is<Function *>() ||
         mReplacementFor.empty() && "Replacement stack must be empty!");
  mReplacementFor.emplace_back();
  LLVM_DEBUG(dbgs() << "[DVMH IPO]: add to replacement stack ("
                    << mReplacementFor.size() << ")\n");
  if (Level.is<Function *>()) {
    LLVM_DEBUG(dbgs() << "[DVMH IPO]: process function "
                      << Level.get<Function *>()->getName() << "\n");
    mToActual.clear();
    mToGetActual.clear();
    mDistinctMemory.clear();
    auto RF{mSocket->getAnalysis<DIEstimateMemoryPass>(F)};
    if (!RF) {
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: disable IPO due to absence of "
                           "metadata-level alias tree for the function"
                        << F.getName() << "\n");
      mReplacementFor.pop_back();
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: extract from replacement stack ("
                        << mReplacementFor.size() << ")\n");
      ParallelCtx.getIPORoot().invalidate();
      return false;
    }
    SmallVector<BasicBlock *, 8> OutermostBlocks;
    if (!OptimizeChildren) {
      mReplacementFor.emplace_back();
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: add to replacement stack ("
                        << mReplacementFor.size() << ")\n");
    } else {
      assert(IPOInfoItr != mIPOMap.end() && "Function must not be optimized!");
      if (!OptimizeAll || IPOInfoItr->get<bool>()) {
        // We will add actualization directives for variables which are not
        // explicitly accessed in the current function, only if IPO is possible
        // or the function is partially located in an optimization region. To
        // ensure IPO is active we will later add corresponding edges between
        // the IPO root node and corresponding directives. Thus, we remember the
        // IPO root node as a replacement target here.
        mReplacementFor.back().get<Sibling>().push_back(
            &ParallelCtx.getIPORoot());
        auto collectForIPO = [this, &CSMemoryMatcher, IPOInfoItr, &findDIM](
                                 auto &IPOToOptimize, auto &LocalToOptimize) {
          for (auto [V, D] : IPOToOptimize) {
            if (IPOInfoItr->get<Value>().contains(V))
              continue;
            VariableT Var{findDIM(V, D)};
            if (!Var.get<MD>())
              continue;
            auto DIMI{CSMemoryMatcher.find<Origin>(&*Var.get<MD>())};
            auto I{
                LocalToOptimize.try_emplace(DIMI->get<Clone>()->getAliasNode())
                    .first};
            if (!is_contained(I->template get<VariableT>(), Var))
              I->template get<VariableT>().push_back(std::move(Var));
            I->template get<Hierarchy>() = mReplacementFor.size() - 1;
          }
        };
        collectForIPO(mIPOToActual, mToActual);
        collectForIPO(mIPOToGetActual, mToGetActual);
      }
      mReplacementFor.emplace_back();
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: add to replacement stack ("
                        << mReplacementFor.size() << ")\n");
      auto &ServerDIAT{RF->value<DIEstimateMemoryPass *>()->getAliasTree()};
      for (auto &DIM :
           make_range(ServerDIAT.memory_begin(), ServerDIAT.memory_end()))
        if (auto *DIUM{dyn_cast<DIUnknownMemory>(&DIM)};
            DIUM && DIUM->isDistinct())
          mDistinctMemory.insert(DIUM->getAliasNode());
      auto collectInParallelBlock =
          [this, &CSMemoryMatcher](ParallelBlock &PB, bool IsExplicitlyNested) {
            auto collect = [this, &CSMemoryMatcher, IsExplicitlyNested](
                               PragmaData *PD, auto &LocalToOptimize) {
              if (PD->isFinal() || PD->isRequired())
                return;
              for (auto &Align : PD->getMemory()) {
                auto &Var{std::get<VariableT>(Align.Target)};
                auto DIMI{CSMemoryMatcher.find<Origin>(&*Var.get<MD>())};
                auto [I, IsNew] = LocalToOptimize.try_emplace(
                    DIMI->get<Clone>()->getAliasNode());
                if (!is_contained(I->template get<VariableT>(), Var))
                  I->template get<VariableT>().push_back(Var);
                if (IsNew || IsExplicitlyNested)
                  I->template get<Hierarchy>() = mReplacementFor.size() - 1;
              }
              if (IsExplicitlyNested)
                mReplacementFor.back().get<Sibling>().push_back(PD);
            };
            for (auto &PI : PB)
              if (auto *Actual{dyn_cast<PragmaActual>(PI.get())})
                collect(Actual, mToActual);
              else if (auto *GetActual{dyn_cast<PragmaGetActual>(PI.get())})
                collect(GetActual, mToGetActual);
          };
      for (auto &BB : F) {
        auto *L{LI.getLoopFor(&BB)};
        if (!L)
          OutermostBlocks.push_back(&BB);
        if (auto ParallelItr{ParallelCtx.getParallelization().find(&BB)};
            ParallelItr != ParallelCtx.getParallelization().end())
          for (auto &PL : ParallelItr->get<ParallelLocation>()) {
            bool IsExplicitlyNested{!L || L && PL.Anchor.is<MDNode *>() &&
                                              PL.Anchor.get<MDNode *>() ==
                                                  L->getLoopID() &&
                                              !L->getParentLoop()};
            collectInParallelBlock(PL.Exit, IsExplicitlyNested);
            collectInParallelBlock(PL.Entry, IsExplicitlyNested);
          }
      }
    }
    if (mToActual.empty() && mToGetActual.empty()) {
      LLVM_DEBUG(if (!OptimizeChildren) dbgs()
                 << "[DVMH IPO]: optimize region boundary function "
                 << F.getName() << "\n");
      // If the  current function is outside optimization regions
      // (OptimizeChildrent == true), we extract actual/get_actual directives
      // from callees and surround corresponding calls.
      // We do the same if there is no memory to optimize in the current
      // function.
      auto &CG{getAnalysis<CallGraphWrapperPass>().getCallGraph()};
      for (auto Call : *CG[&F]) {
        if (!Call.second->getFunction() || !Call.first ||
            !isa_and_nonnull<Instruction>(*Call.first))
          continue;
        if (!needToOptimize(*Call.second->getFunction()).first)
          continue;
        collectInCallee(cast<Instruction>(*Call.first),
                        Call.second->getFunction(), true);
      }
      mReplacementFor.pop_back();
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: extract from replacement stack ("
                        << mReplacementFor.size() << ")\n");
      mReplacementFor.pop_back();
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: extract from replacement stack ("
                        << mReplacementFor.size() << ")\n");
      return false;
    }
    assert(IPOInfoItr != mIPOMap.end() && "Function must not be optimized!");
    // Fetch directives from callees if IPO is disabled for the current
    // function.
    if (!IPOInfoItr->get<bool>())
      for (auto *BB : OutermostBlocks) {
        collectInCallees(*BB, !OptimizeAll);
      }
      LLVM_DEBUG(
          dbgs() << "[DVMH IPO]: number of directives to replace at a level "
                 << mReplacementFor.size() << " is "
                 << mReplacementFor.back().get<Sibling>().size() << "\n");
    return true;
  }
  assert(IPOInfoItr != mIPOMap.end() && "Function must not be optimized!");
  if (Level.is<Loop *>()) {
    auto initializeReplacementLevel = [this,
                                       &CSMemoryMatcher](ParallelBlock &PB) {
      auto initHierarchy = [this, CSMemoryMatcher](PragmaData *PD,
                                                   auto &LocalToOptimize) {
        if (PD->isFinal() || PD->isRequired())
          return;
        for (auto &Align : PD->getMemory()) {
          auto &Var{std::get<VariableT>(Align.Target)};
          auto DIMI{CSMemoryMatcher.find<Origin>(&*Var.get<MD>())};
          auto I{LocalToOptimize.find(DIMI->get<Clone>()->getAliasNode())};
          I->template get<Hierarchy>() = mReplacementFor.size() - 1;
        }
        mReplacementFor.back().get<Sibling>().push_back(PD);
      };
      for (auto &PI : PB)
        if (auto *Actual{dyn_cast<PragmaActual>(PI.get())})
          initHierarchy(Actual, mToActual);
        else if (auto *GetActual{dyn_cast<PragmaGetActual>(PI.get())})
          initHierarchy(GetActual, mToGetActual);
    };
    bool NeedToOptimize{needToOptimize(*Level.get<Loop *>())};
    for (auto *BB : Level.get<Loop *>()->blocks()) {
      auto *L{LI.getLoopFor(BB)};
      assert(L && "At least one loop must contain the current basic block!");
      if (auto ParallelItr{ParallelCtx.getParallelization().find(BB)};
          ParallelItr != ParallelCtx.getParallelization().end())
        for (auto &PL : ParallelItr->get<ParallelLocation>()) {
          // Process only blocks which are nested explicitly in the current
          // level.
          if (Level.get<Loop *>() == L ||
              PL.Anchor.is<MDNode *>() &&
                  PL.Anchor.get<MDNode *>() == L->getLoopID() &&
                  Level.get<Loop *>() == L->getParentLoop()) {
            initializeReplacementLevel(PL.Entry);
            initializeReplacementLevel(PL.Exit);
          }
        }
      // Fetch directives from callees if IPO is disabled for the current
      // function.
      if (Level.get<Loop *>() == L && !IPOInfoItr->get<bool>())
        collectInCallees(*BB, !NeedToOptimize);
    }
  }
  LLVM_DEBUG(dbgs() << "[DVMH IPO]: number of directives to replace at a level "
                    << mReplacementFor.size() << " is "
                    << mReplacementFor.back().get<Sibling>().size() << "\n");
  return true;
}

bool DVMHDataTransferIPOPass::optimizeGlobalOut(
    PointerUnion<Loop *, Function *> Level,
    const DVMHDataTransferIPOPassProvider &Provider) {
  LLVM_DEBUG(dbgs() << "[DVMH IPO]: global optimization, visit level upward\n");
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  auto &F{Level.is<Function *>()
              ? *Level.get<Function *>()
              : *Level.get<Loop *>()->getHeader()->getParent()};
  auto IPOInfoItr{mIPOMap.find(&F)};
  assert(IPOInfoItr != mIPOMap.end() && "Function must not be optimized!");
  auto RM{mSocket->getAnalysis<AnalysisClientServerMatcherWrapper,
                             ClonedDIMemoryMatcherWrapper>()};
  auto &ClientToServer{**RM->value<AnalysisClientServerMatcherWrapper *>()};
  auto &ServerF{cast<Function>(*ClientToServer[&F])};
  auto &CSMemoryMatcher{
      *(**RM->value<ClonedDIMemoryMatcherWrapper *>())[ServerF]};
  // Map from induction variable to a list of headers of top-level loops
  // in parallel nests which contain this induction variable.
  DenseMap<const DIMemory *, TinyPtrVector<BasicBlock *>> ParallelLoops;
  auto collectParallelInductions = [&ParallelCtx,
                                    &ParallelLoops](BasicBlock &BB) {
    auto collectForParallelBlock = [&BB, &ParallelLoops](ParallelBlock &PB,
                                                         auto Anchor) {
      for (auto &PI : PB)
        if (auto *Parallel{dyn_cast<PragmaParallel>(PI.get())})
          for (auto &I : Parallel->getClauses().get<trait::Induction>())
            ParallelLoops.try_emplace(I.get<VariableT>().get<MD>())
                .first->second.push_back(&BB);
    };
    if (auto ParallelItr{ParallelCtx.getParallelization().find(&BB)};
        ParallelItr != ParallelCtx.getParallelization().end())
      for (auto &PL : ParallelItr->get<ParallelLocation>()) {
        collectForParallelBlock(PL.Entry, PL.Anchor);
        collectForParallelBlock(PL.Exit, PL.Anchor);
      }
  };
  auto &TLI{getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(F)};
  auto RF{mSocket->getAnalysis<DIEstimateMemoryPass>(F)};
  if (!RF)
    return false;
  auto &ServerDIAT{RF->value<DIEstimateMemoryPass *>()->getAliasTree()};
  SpanningTreeRelation<const DIAliasTree *> ServerSTR{&ServerDIAT};
  auto initPragmaData = [this, &ParallelCtx,
                         &DL = F.getParent()->getDataLayout(), IPOInfoItr](
                            auto &Data, auto &IPOVars,
                            IPOMap::iterator IPOCalleeItr, PragmaData &D) {
    bool ActiveIPO{IPOCalleeItr != mIPOMap.end() && IPOCalleeItr->get<bool>()};
    bool IPOVariablesOnly{true};
    for (auto &Var : Data.template get<VariableT>()) {
      // Check whether this variable participates in IPO.
      auto IsIPOVar{false};
      if (auto MH{Var.template get<MD>()}; MH && !MH->emptyBinding())
        if (auto BindItr{MH->begin()}; *BindItr)
          if (auto Ptr{getUnderlyingObject(*BindItr, 0)};
              IPOVars.count(Ptr) && (IPOCalleeItr == mIPOMap.end() ||
                                     !IPOCalleeItr->get<Value>().count(Ptr)))
            IsIPOVar = true;
      // Do not actualize a variable accessed in a callee if it is not
      // directly accessed in a function and can be optimized inside callee.
      // If IPO is enabled this variable will be actualized separately.
      if (IsIPOVar && ActiveIPO && Data.template get<Hierarchy>() == 0)
        continue;
      IPOVariablesOnly &= IsIPOVar;
      D.getMemory().emplace(Var);
    }
    if (D.getMemory().empty()) {
      D.skip();
      return;
    }
    mReplacementFor[Data.template get<Hierarchy>()]
        .template get<Hierarchy>()
        .push_back(&D);
    if (ActiveIPO && IPOVariablesOnly) {
      assert(Data.template get<Hierarchy>() != 0 &&
             "Cycle in a replacement tree!");
      if (IPOInfoItr->get<bool>()) {
        // Do not attach this directive to callee if IPO is possible.
        if (!is_contained(D.children(), &ParallelCtx.getIPORoot())) {
          D.child_insert(&ParallelCtx.getIPORoot());
          ParallelCtx.getIPORoot().parent_insert(&D);
        }
      } else {
        // If IPO is disabled for the current function, then this directive is
        // not necessary. If IPO is enabled for the entire program, another
        // directive has been already inserted instead. If IPO is disabled for
        // the entire program the intraprocedural optimization will be disabled
        // for this function as well (limitation of the current implementation),
        // so this directive is redundant.
        D.skip();
      }
    } else {
      D.finalize();
    }
  };
  auto addGetActualIf =
      [this, &ParallelCtx, &ServerSTR,
       &initPragmaData](Instruction &I, const DIAliasNode *AliasWith,
                        IPOMap::iterator IPOCalleeItr,
                        SmallPtrSetImpl<const DIAliasNode *> &InsertedList,
                        PragmaGetActual *GetActual = nullptr) {
        if (auto I{find_if(mToGetActual,
                           [&ServerSTR, AliasWith, &InsertedList](auto &Data) {
                             return !InsertedList.count(
                                        Data.template get<DIAliasNode>()) &&
                                    !ServerSTR.isUnreachable(
                                        AliasWith,
                                        Data.template get<DIAliasNode>());
                           })};
            I == mToGetActual.end())
          return;
        if (!GetActual) {
          auto GetActualRef{
              ParallelCtx.getParallelization().emplace<PragmaGetActual>(
                  I.getParent(), &I, true /*OnEntry*/, false /*IsRequired*/,
                  false /*IsFinal*/)};
          GetActual = cast<PragmaGetActual>(GetActualRef);
        }
        for (auto &Data : mToGetActual) {
          if (InsertedList.count(Data.get<DIAliasNode>()) ||
              ServerSTR.isUnreachable(AliasWith, Data.get<DIAliasNode>()))
            continue;
          InsertedList.insert(Data.get<DIAliasNode>());
          initPragmaData(Data, mIPOToGetActual, IPOCalleeItr, *GetActual);
        }
      };
  auto addTransferToWrite =
      [this, &ParallelCtx, &ServerSTR, &addGetActualIf, &initPragmaData](
          Instruction &I, const DIAliasNode *CurrentAN,
          IPOMap::iterator IPOCalleeItr,
          SmallPtrSetImpl<const DIAliasNode *> &InsertedGetActuals) {
        if (auto I{find_if(mToActual,
                           [&ServerSTR, CurrentAN](auto &Data) {
                             return !ServerSTR.isUnreachable(
                                 CurrentAN, Data.template get<DIAliasNode>());
                           })};
            I == mToActual.end())
          return;
        auto ActualRef{ParallelCtx.getParallelization().emplace<PragmaActual>(
            I.getParent(), &I, false /*OnEntry*/, false /*IsRequired*/,
            false /*IsFinal*/)};
        auto *Actual{cast<PragmaActual>(ActualRef)};
        auto GetActualRef{
            ParallelCtx.getParallelization().emplace<PragmaGetActual>(
                I.getParent(), &I, true /*OnEntry*/, false /*IsRequired*/,
                false /*IsFinal*/)};
        auto *GetActual{cast<PragmaGetActual>(GetActualRef)};
        for (auto &Data : mToActual) {
          if (ServerSTR.isUnreachable(CurrentAN, Data.get<DIAliasNode>()))
            continue;
          initPragmaData(Data, mIPOToActual, IPOCalleeItr, *Actual);
          addGetActualIf(I, Data.get<DIAliasNode>(), IPOCalleeItr,
                         InsertedGetActuals, GetActual);
        }
        if (cast<PragmaData>(GetActualRef)->getMemory().empty())
          cast<PragmaData>(GetActualRef)->skip();
      };
  auto processBB = [this, Level, &ParallelLoops, &TLI, &CSMemoryMatcher,
                    &Provider, &addGetActualIf,
                    &addTransferToWrite](BasicBlock &BB) {
    auto &LI{Provider.get<LoopInfoWrapperPass>().getLoopInfo()};
    // Process blocks which are only nested explicitly in the current level.
    if (auto *L{LI.getLoopFor(&BB)};
        !(!L && Level.is<Function *>() || L && Level.dyn_cast<Loop *>() == L))
      return;
    for (auto &I : BB) {
      auto IPOCalleeItr{mIPOMap.end()};
      if (auto Call{dyn_cast<CallBase>(&I)})
        if (auto Callee{llvm::dyn_cast<Function>(
                Call->getCalledOperand()->stripPointerCasts())})
          IPOCalleeItr = mIPOMap.find(Callee);
      SmallPtrSet<const DIAliasNode *, 1> InsertedGetActuals;
      for_each_memory(
          I, TLI,
          [this, &Level, &ParallelLoops, &Provider, &CSMemoryMatcher,
           IPOCalleeItr, &addGetActualIf, &addTransferToWrite,
           &InsertedGetActuals](Instruction &I, MemoryLocation &&Loc,
                                unsigned OpIdx, AccessInfo IsRead,
                                AccessInfo IsWrite) {
            if (IsRead == AccessInfo::No && IsWrite == AccessInfo::No)
              return;
            auto &AT{Provider.get<EstimateMemoryPass>().getAliasTree()};
            auto &DIAT{
                Provider.get<DIEstimateMemoryPass>().getAliasTree()};
            auto &DT{
                Provider.get<DominatorTreeWrapperPass>().getDomTree()};
            auto &F{*I.getFunction()};
            const auto &DL{F.getParent()->getDataLayout()};
            auto *EM{AT.find(Loc)};
            assert(EM && "Estimate memory must be presented in alias tree!");
            auto RawDIM{getRawDIMemoryIfExists(*EM->getTopLevelParent(),
                                               F.getContext(), DL, DT)};
            if (!RawDIM) {
              for (auto *AN : mDistinctMemory) {
                if (IsWrite != AccessInfo::No)
                  addTransferToWrite(I, AN, IPOCalleeItr, InsertedGetActuals);
                if (IsRead != AccessInfo::No)
                  addGetActualIf(I, AN, IPOCalleeItr, InsertedGetActuals);
              }
              return;
            }
            auto DIMItr{DIAT.find(*RawDIM)};
            assert(DIMItr != DIAT.memory_end() &&
                   "Existing memory must be presented in metadata alias tree.");
            auto anyLoopPostDominates =
                [&ParallelLoops,
                 &PDT = Provider.get<PostDominatorTreeWrapperPass>()
                            .getPostDomTree()](DIMemory *DIM,
                                               const BasicBlock *WhatBB) {
                  if (auto ParallelItr{ParallelLoops.find(DIM)};
                      ParallelItr != ParallelLoops.end())
                    for (auto *BB : ParallelItr->second)
                      if (PDT.dominates(BB, WhatBB))
                        return true;
                  return false;
                };
            auto CSMemoryMatchItr{CSMemoryMatcher.find<Origin>(&*DIMItr)};
            DIAliasNode *CurrentAN{
                CSMemoryMatchItr->get<Clone>()->getAliasNode()};
            if (IsWrite != AccessInfo::No) {
              // Do not transfer induction variables explicitly if there is a
              // parallel loop which post-dominates induction variable access.
              if (anyLoopPostDominates(&*DIMItr, I.getParent()))
                return;
              addTransferToWrite(I, CurrentAN, IPOCalleeItr, InsertedGetActuals);
            }
            if (IsRead != AccessInfo::No)
              addGetActualIf(I, CurrentAN, IPOCalleeItr, InsertedGetActuals);
          },
          [this, &Provider, &CSMemoryMatcher, IPOCalleeItr, &addGetActualIf,
           &addTransferToWrite, &InsertedGetActuals](
              Instruction &I, AccessInfo IsRead, AccessInfo IsWrite) {
            if (IsRead == AccessInfo::No && IsWrite == AccessInfo::No)
              return;
            auto &AT{Provider.get<EstimateMemoryPass>().getAliasTree()};
            auto *AN{AT.findUnknown(I)};
            if (!AN)
              return;
            auto &DIAT{
                Provider.get<DIEstimateMemoryPass>().getAliasTree()};
            auto &DT{
                Provider.get<DominatorTreeWrapperPass>().getDomTree()};
            auto RawDIM{getRawDIMemoryIfExists(I, I.getContext(), DT)};
            if (!RawDIM) {
              for (auto *AN : mDistinctMemory) {
                if (IsWrite != AccessInfo::No)
                  addTransferToWrite(I, AN, IPOCalleeItr, InsertedGetActuals);
                if (IsRead != AccessInfo::No)
                  addGetActualIf(I, AN, IPOCalleeItr, InsertedGetActuals);
              }
              return;
            }
            auto DIMItr{DIAT.find(*RawDIM)};
            assert(DIMItr != DIAT.memory_end() &&
                   "Existing memory must be presented in metadata alias tree.");
            auto CSMemoryMatchItr{CSMemoryMatcher.find<Origin>(&*DIMItr)};
            DIAliasNode *CurrentAN{
                CSMemoryMatchItr->get<Clone>()->getAliasNode()};
            if (IsWrite != AccessInfo::No)
              addTransferToWrite(I, CurrentAN, IPOCalleeItr,
                                 InsertedGetActuals);
            if (IsRead != AccessInfo::No)
              addGetActualIf(I, CurrentAN, IPOCalleeItr, InsertedGetActuals);
          });
    }
  };
  LLVM_DEBUG(dbgs() << "[DVMH IPO]: optimize level " << mReplacementFor.size()
                    << "\n");
  // Normalize replacement level numbers which specify replacement levels which
  // depends on corresponding memory locations.
  for (auto &Data : mToActual)
    Data.get<Hierarchy>() =
        std::min<unsigned>(Data.get<Hierarchy>(), mReplacementFor.size() - 1);
  for (auto &Data : mToGetActual)
    Data.get<Hierarchy>() =
        std::min<unsigned>(Data.get<Hierarchy>(), mReplacementFor.size() - 1);
  SmallVector<ParallelItem *, 2> ConservativeReplacements;
  if (Level.is<Function *>()) {
    LLVM_DEBUG(dbgs() << "[DVMH IPO]: finalize function "
                      << Level.get<Function *>()->getName() << "\n");
    LLVM_DEBUG(
      dbgs() << "[DVMH IPO]: local actual: ";
      for (auto &Data : mToActual)
        for (auto &Var : Data.template get<VariableT>())
          dbgs() << Var.get<AST>()->getName() << " ";
      dbgs() << "\n";
      dbgs() << "[DVMH IPO]: local get_actual: ";
      for (auto &Data : mToGetActual)
        for (auto &Var : Data.template get<VariableT>())
          dbgs() << Var.get<AST>()->getName() << " ";
      dbgs() << "\n";
    );
    if (!needToOptimize(*Level.get<Function *>()).first) {
      for (auto *PL: mReplacementFor.back().get<Sibling>())
        PL->finalize();
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: extract from replacement stack ("
                        << mReplacementFor.size() << ")\n");
      mReplacementFor.pop_back();
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: extract from replacement stack ("
                        << mReplacementFor.size() << ")\n");
      mReplacementFor.pop_back();
      return false;
    }
    for (auto &BB : *Level.get<Function *>())
      collectParallelInductions(BB);
    // We conservatively actualize memory, which is available outside the
    // function, at the function entry point.
    if (!mReplacementFor.back().get<Sibling>().empty()) {
      auto ActiveIPO{IPOInfoItr != mIPOMap.end() && IPOInfoItr->get<bool>()};
      auto &EntryBB{F.getEntryBlock()};
      auto addIfNeed = [&ConservativeReplacements,
                        &DL = F.getParent()->getDataLayout(),
                        &IPOInfoItr](auto &Memory, auto &IPOMemory,
                                     auto *FinalPragma, auto *IPOPragma) {
        for (auto &Data : Memory)
          for (auto &Var : Data.template get<VariableT>()) {
            if (auto *DIEM{
                    dyn_cast<DIEstimateMemory>(Var.template get<MD>())}) {
              auto *V{DIEM->getVariable()};
              if (isa<DIGlobalVariable>(V) ||
                  cast<DILocalVariable>(V)->isParameter()) {
                if (auto BindItr{DIEM->begin()}; FinalPragma != IPOPragma &&
                                                 !DIEM->emptyBinding() &&
                                                 *BindItr)
                  if (auto Ptr{getUnderlyingObject(*BindItr, 0)};
                      IPOMemory.count(Ptr) &&
                      !IPOInfoItr->get<Value>().count(Ptr))
                    IPOPragma->getMemory().emplace(Var);
                  else
                    FinalPragma->getMemory().emplace(Var);
                else
                  FinalPragma->getMemory().emplace(Var);
              }
            } else {
              FinalPragma->getMemory().emplace(Var);
            }
          }
        if (!FinalPragma->getMemory().empty())
          ConservativeReplacements.push_back(FinalPragma);
        if (IPOPragma != FinalPragma && !IPOPragma->getMemory().empty()) {
          ConservativeReplacements.push_back(IPOPragma);
          IPOInfoItr->get<std::remove_pointer_t<decltype(IPOPragma)>>()
              .push_back(IPOPragma);
        }
      };
      auto ActualRef{ParallelCtx.getParallelization().emplace<PragmaActual>(
          &EntryBB, &F, true /*OnEntry*/, false /*IsRequired*/,
          true /*IsFinal*/)};
      auto *FinalActual{cast<PragmaActual>(ActualRef)};
      auto *IPOActual{FinalActual};
      if (ActiveIPO) {
        auto IPOActualRef{
            ParallelCtx.getParallelization().emplace<PragmaActual>(
                &EntryBB, &F, true /*OnEntry*/, false /*IsRequired*/,
                false /*IsFinal*/)};
        IPOActual = cast<PragmaActual>(IPOActualRef);
      }
      addIfNeed(mToActual, mIPOToActual, FinalActual, IPOActual);
      // We conservatively copy memory from the device before the exit from the
      // function.
      for (auto &I : instructions(F)) {
        if (!isa<ReturnInst>(I))
          continue;
        auto GetActualRef{
            ParallelCtx.getParallelization().emplace<PragmaGetActual>(
                I.getParent(), &I, true /*OnEntry*/, false /*IsRequired*/,
                true /*IsFinal*/)};
        auto *FinalGetActual{cast<PragmaGetActual>(GetActualRef)};
        auto *IPOGetActual{FinalGetActual};
        if (ActiveIPO) {
          auto IPOGetActualRef{
              ParallelCtx.getParallelization().emplace<PragmaGetActual>(
                  I.getParent(), &I, true /*OnEntry*/, false /*IsRequired*/,
                  false /*IsFinal*/)};
          IPOGetActual = cast<PragmaGetActual>(IPOGetActualRef);
        }
        addIfNeed(mToGetActual, mIPOToGetActual, FinalGetActual, IPOGetActual);
      }
      if (Level.get<Function *>() == mEntryPoint)
        for (auto *PI : ConservativeReplacements)
          cast<PragmaData>(PI)->skip();
    }
    // Process blocks inside the function and add `actual/get_actual` to
    // instructions which access memory.
    for (auto &BB : *Level.get<Function *>())
      processBB(BB);
  } else {
    if (!needToOptimize(*Level.get<Loop *>())) {
      for (auto *PL: mReplacementFor.back().get<Sibling>())
        PL->finalize();
      mReplacementFor.pop_back();
      LLVM_DEBUG(dbgs() << "[DVMH IPO]: extract from replacement stack ("
                        << mReplacementFor.size() << ")\n");
      return true;
    }
    for (auto *BB : Level.get<Loop *>()->blocks())
      collectParallelInductions(*BB);
    if (!mReplacementFor.back().get<Sibling>().empty()) {
      auto HeaderBB = Level.get<Loop *>()->getHeader();
      // We conservatively copy memory to the device, on the entry to the loop.
      for (auto *BB : children<Inverse<BasicBlock *>>(HeaderBB)) {
        if (Level.get<Loop *>()->contains(BB))
          continue;
        auto ActualRef{ParallelCtx.getParallelization().emplace<PragmaActual>(
            BB, BB->getTerminator(), true /*OnEntry*/, false /*IsRequired*/)};
        for (auto &Data : mToActual)
          if (Data.get<Hierarchy>() == mReplacementFor.size() - 1)
            for (auto &Var : Data.get<VariableT>())
              cast<PragmaActual>(ActualRef)->getMemory().emplace(Var);
        if (!cast<PragmaActual>(ActualRef)->getMemory().empty())
          ConservativeReplacements.push_back(ActualRef.getUnchecked());
      }
      // We conservatively copy memory from the device on the exit from the
      // loop.
      SmallVector<BasicBlock *, 4> ExitBlocks;
      Level.get<Loop *>()->getExitBlocks(ExitBlocks);
      for (auto *BB : ExitBlocks) {
        auto Inst{BB->begin()};
        for (; Inst->mayReadFromMemory() || &*Inst != BB->getTerminator();
             ++Inst)
          ;
        auto GetActualRef{
            ParallelCtx.getParallelization().emplace<PragmaGetActual>(
                BB, &*Inst, true /*OnEntry*/, false /*IsRequired*/)};
        for (auto &Data : mToGetActual)
          if (Data.get<Hierarchy>() == mReplacementFor.size() - 1)
            for (auto &Var : Data.get<VariableT>())
              cast<PragmaGetActual>(GetActualRef)->getMemory().emplace(Var);
        if (!cast<PragmaGetActual>(GetActualRef)->getMemory().empty())
          ConservativeReplacements.push_back(GetActualRef.getUnchecked());
      }
    }
    // Process blocks inside the function and add `actual/get_actual` to
    // instructions which access memory.
    for (auto *BB : Level.get<Loop *>()->blocks())
      processBB(*BB);
  }
  LLVM_DEBUG(
      dbgs() << "[DVMH IPO]: number of directives to optimize this level is "
             << mReplacementFor.back().get<Sibling>().size() << "\n");
  LLVM_DEBUG(dbgs() << "[DVMH IPO]: size of replacement target is "
                    << mReplacementFor.back().get<Hierarchy>().size() << "\n");
  // Update replacement relation after all inner levels have benn processed.
  for (auto *PL : mReplacementFor.back().get<Sibling>())
    for (auto *ToReplace : mReplacementFor.back().get<Hierarchy>()) {
      PL->child_insert(ToReplace);
      ToReplace->parent_insert(PL);
    }
  LLVM_DEBUG(dbgs() << "[DVMH IPO]: conservative replacement size is "
                    << ConservativeReplacements.size() << "\n");
  // The created directives are necessary to remove optimized ones. So, we
  // update replacement relation.
  for (auto *PL : mReplacementFor.back().get<Sibling>())
    for (auto *ToReplace : ConservativeReplacements) {
      PL->child_insert(ToReplace);
      ToReplace->parent_insert(PL);
    }
  mReplacementFor.pop_back();
  LLVM_DEBUG(dbgs() << "[DVMH IPO]: extract from replacement stack ("
                    << mReplacementFor.size() << ")\n");
  if (!Level.is<Function *>()) {
    for (auto *ToReplace : ConservativeReplacements)
      mReplacementFor.back().get<Sibling>().push_back(
          cast<ParallelLevel>(ToReplace));
  } else {
    if (!mReplacementFor.back().get<Sibling>().empty()) {
      for (auto *PL : mReplacementFor.back().get<Sibling>())
        for (auto *ToReplace : mReplacementFor.back().get<Hierarchy>()) {
          PL->child_insert(ToReplace);
          ToReplace->parent_insert(PL);
        }
      for (auto *PL : ConservativeReplacements) {
        if (PL->isFinal())
          continue;
        for (auto *ToReplace : mReplacementFor.back().get<Sibling>()) {
          cast<ParallelLevel>(PL)->child_insert(ToReplace);
          ToReplace->parent_insert(PL);
        }
      }
    }
    mReplacementFor.pop_back();
    LLVM_DEBUG(dbgs() << "[DVMH IPO]: extract from replacement stack ("
                      << mReplacementFor.size() << ")\n");
  }
  return true;
}

bool DVMHDataTransferIPOPass::runOnModule(Module &M) {
  releaseMemory();
  if (!(mEntryPoint = M.getFunction("main")))
    mEntryPoint = M.getFunction("MAIN_");
  // TODO (kaniandr@gmail.com): emit warning if entry point has not be found
  // TODO (kaniandr@gmail.com): add option to manually specify entry point
  auto &GO{getAnalysis<GlobalOptionsImmutableWrapper>().getOptions()};
  auto &RegionInfo{getAnalysis<ClangRegionCollector>().getRegionInfo()};
  if (GO.OptRegions.empty()) {
    transform(RegionInfo, std::back_inserter(mRegions),
              [](const OptimizationRegion &R) { return &R; });
  } else {
    for (auto &Name : GO.OptRegions)
      if (auto *R{RegionInfo.get(Name)})
        mRegions.push_back(R);
  }
  auto &CG{getAnalysis<CallGraphWrapperPass>().getCallGraph()};
  unsigned SCCId{0};
  for (scc_iterator<CallGraph *> I{scc_begin(&CG)}; !I.isAtEnd(); ++I, ++SCCId)
    for (auto *CGN : *I)
      if (auto F{CGN->getFunction()}) {
        if (F->isIntrinsic() || hasFnAttr(*F, AttrKind::LibFunc))
          continue;
        mFunctions.insert(std::pair{F, std::tuple{SCCId, I.hasCycle()}});
      }
  auto &SocketInfo{getAnalysis<AnalysisSocketImmutableWrapper>()};
  mSocket = &SocketInfo->getActive()->second;
  DVMHDataTransferIPOPassProvider::initialize<GlobalOptionsImmutableWrapper>(
      [&GO](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(&GO);
      });
  auto &GlobalsAA{getAnalysis<GlobalsAAWrapperPass>().getResult()};
  DVMHDataTransferIPOPassProvider::initialize<
      GlobalsAAResultImmutableWrapper>(
      [&GlobalsAA](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(GlobalsAA);
      });
  auto &DIMEnv{getAnalysis<DIMemoryEnvironmentWrapper>().get()};
  DVMHDataTransferIPOPassProvider::initialize<DIMemoryEnvironmentWrapper>(
      [&DIMEnv](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(DIMEnv);
      });
  if (auto &GAP{getAnalysis<GlobalsAccessWrapper>()})
    DVMHDataTransferIPOPassProvider::initialize<GlobalsAccessWrapper>(
        [&GAP](GlobalsAccessWrapper &Wrapper) { Wrapper.set(*GAP); });
  if (!initializeIPO(M))
    return false;
  auto &ParallelCtx{getAnalysis<DVMHParallelizationContext>()};
  for (auto &&[F, Node] : mFunctions) {
    if (F->isDeclaration())
      continue;
    if (ParallelCtx.isParallelCallee(*F))
      continue;
    auto &Provider{getAnalysis<DVMHDataTransferIPOPassProvider>(*F)};
    auto &LI{Provider.get<LoopInfoWrapperPass>().getLoopInfo()};
    optimizeUpward(F, LI.begin(), LI.end(), Provider);
  }
  return false;
}

INITIALIZE_PROVIDER(DVMHDataTransferIPOPassProvider, "dvmh-actual-ipo-provider",
  "DVMH Actual/Get Actual IPO (Provider)")

char DVMHDataTransferIPOPass::ID = 0;
INITIALIZE_PASS_BEGIN(DVMHDataTransferIPOPass, "dvmh-actual-ipo",
                      "DVMH Actual/Get Actual IPO", false, false)
INITIALIZE_PASS_DEPENDENCY(AnalysisSocketImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ClangRegionCollector)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DVMHParallelizationContext)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
INITIALIZE_PASS_DEPENDENCY(GlobalsAccessWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DVMHDataTransferIPOPassProvider)
INITIALIZE_PASS_END(DVMHDataTransferIPOPass, "dvmh-actual-ipo",
                    "DVMH Actual/Get Actual IPO", false, false)

ModulePass *llvm::createDVMHDataTransferIPOPass() {
  return new DVMHDataTransferIPOPass;
}
