//===-- DVMHAutoPar.cpp --- DVMH Based Parallelization (Clang) ---*- C++ -*===//
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
// This file implements a pass to perform DVMH-based auto parallelization.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/DFRegionInfo.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Analysis/Clang/RegionDirectiveInfo.h"
#include "tsar/Analysis/Clang/MemoryMatcher.h"
#include "tsar/Analysis/Clang/PerfectLoop.h"
#include "tsar/Analysis/Memory/DIArrayAccess.h"
#include "tsar/Analysis/Memory/DIDependencyAnalysis.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryTraitUtils.h"
#include "tsar/Analysis/Memory/PassAAProvider.h"
#include "tsar/Analysis/Parallel/ParallelLoop.h"
#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/PrintUtils.h"
#include "tsar/Core/Query.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Support/Clang/Diagnostic.h"
#include "tsar/Support/GlobalOptions.h"
#include "tsar/Support/IRUtils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Transform/Clang/Passes.h"
#include "tsar/Unparse/Utils.h"
#include <clang/AST/ASTContext.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/iterator.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/ManagedStatic.h>
#include <bcl/utility.h>
#include <bcl/Json.h>
#include <lp_solve/lp_lib.h>

#undef max
#undef min

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-dvmh-parallel"

using namespace tsar;
using namespace llvm;

namespace tsar {
namespace trait {
JSON_OBJECT_BEGIN(Hardware)
  JSON_OBJECT_PAIR_3(Hardware
    , Latency, double
    , Bandwidth, double
    , ComputationRate, double
  )
  Hardware() : JSON_INIT(Hardware
    , 0.000'001 /*s.*/
    , 56'000'000'000 /*bit/s*/
    , 55'000'000'000 /*flops*/
  ) {}
JSON_OBJECT_END(Hardware)

JSON_OBJECT_BEGIN(Software)
  JSON_OBJECT_PAIR(Software
    , TripCount, uint64_t
  )
  Software() : JSON_INIT(Software
    , 1
  ) {}
JSON_OBJECT_END(Software)

JSON_OBJECT_BEGIN(Weights)
  JSON_OBJECT_ROOT_PAIR_2(Weights
    , Hardware, trait::Hardware
    , Software, trait::Software
  )
  Weights() : JSON_INIT_ROOT {}
JSON_OBJECT_END(Weights)
}
}

JSON_DEFAULT_TRAITS(tsar::trait::, Hardware)
JSON_DEFAULT_TRAITS(tsar::trait::, Software)
JSON_DEFAULT_TRAITS(tsar::trait::, Weights)

namespace {
struct APIntLess {
  bool operator()(const APInt &LHS, const APInt &RHS) const {
    return LHS.slt(RHS);
  }
};

using MILPColumnT = int;
using MILPValueT = REAL;

static inline auto addConstraintex(lprec *LP, ArrayRef<MILPColumnT> Columns,
                                   ArrayRef<MILPValueT> Multipliers,
                                   int Constraint, MILPValueT Constant) {
  return add_constraintex(
      LP, Columns.size(), const_cast<MILPValueT *>(Multipliers.data()),
      const_cast<MILPColumnT *>(Columns.data()), Constraint, Constant);
};

static ManagedStatic<MILPColumnT> FreeColumn;

static inline MILPColumnT getFreeColumn() {
  return (*FreeColumn)++;
}

class ShadowRange {
public:
  ShadowRange() = default;

  const APInt &min() const {
    assert(!empty() && "Range must be empty!");
    return *mShadows.begin();
  }

  const APInt &max() const {
    assert(!empty() && "Range must be empty!");
    auto I = mShadows.begin(), EI = mShadows.end();
    auto Prev = I;
    for (++I, EI = mShadows.end(); I != EI; ++Prev, ++I)
      ;
    return *Prev;
  }

  void insert(const APInt &Shadow) {
    if (is_contained(mShadows, Shadow))
      return;
    mShadows.emplace_back(Shadow);
    sort(mShadows, APIntLess{});
  }

  bool empty() const { return mShadows.empty(); }
  auto size() const { return mShadows.size(); }

private:
  SmallVector<APInt, 8> mShadows;
};

struct RemoteData {
  struct MILPShadow {
    MILPColumnT Column;
    MILPColumnT ChoiceColumn;
  };

  struct MILPRemote {
    MILPColumnT Column;
    MILPColumnT InverseColumn;
  };

  RemoteData() {
    Remote.Column = getFreeColumn();
    Remote.InverseColumn = getFreeColumn();
    Left.Column = getFreeColumn();
    Left.ChoiceColumn = getFreeColumn();
    Right.Column = getFreeColumn();
    Right.ChoiceColumn = getFreeColumn();
  }

  MILPShadow Left;
  MILPShadow Right;
  MILPRemote Remote;
  ShadowRange Shadows;
};

template<unsigned BitWidth = 64>
class DimensionAccessBase {
public:
  static constexpr unsigned getBitWidth() noexcept {
    return BitWidth;
  }

  DimensionAccessBase(unsigned Dimension, MILPColumnT DimensionID)
      : mColumn(DimensionID), mDimension(Dimension) {}

  void add(const APInt &Constant, bool IsWrite) {
    auto B = Constant.sextOrSelf(BitWidth);
    mBounds.first =
        APIntOps::smin(mBounds.first, Constant.sextOrSelf(BitWidth));
    mBounds.second =
        APIntOps::smax(mBounds.second, Constant.sextOrSelf(BitWidth));
    mConstantAccesses.insert(B);
    if (IsWrite) {
      mWrite.reset();
      mHasWriteConflict = true;
    }
  }

  void add(const APInt &Multiplier, const APInt &Constant,
           const APInt &ColumnBound, ObjectID LoopID, bool IsWrite) {
    addInductionForLoop(LoopID);
    auto A = Multiplier.sextOrSelf(BitWidth);
    auto B = Constant.sextOrSelf(BitWidth);
    auto C = ColumnBound.sextOrSelf(BitWidth);
    auto Low = B;
    auto High = (C - 1) * A + B;
    if (A.isNegative())
      std::swap(Low, High);
    mBounds.first = APIntOps::smin(mBounds.first, Low);
    mBounds.second = APIntOps::smax(mBounds.second, High);
    auto LoopItr = find(LoopID);
    if (LoopItr == mAccesses.end()) {
      mAccesses.emplace_back();
      LoopItr = mAccesses.end() - 1;
      LoopItr->get<Loop>() = LoopID;
    }
    LoopItr->get<RemoteData>()[A].Shadows.insert(B);
    if (IsWrite) {
      if (mWrite) {
        if (mWrite->Multiplier != A || mWrite->Constant != B ||
            mWrite->LoopID != LoopID) {
          mHasWriteConflict = true;
          mWrite.reset();
        }
      } else {
        if (A.isNullValue())
          mHasWriteConflict = true;
        else
          mWrite = AffineAccess{ LoopID, A, B };
      }
    }
  }

  auto begin() { return mAccesses.begin(); }
  auto end() { return mAccesses.end(); }

  auto begin() const { return mAccesses.begin(); }
  auto end() const { return mAccesses.end(); }

  bool empty() const noexcept { return mAccesses.empty(); }

  auto find(ObjectID LoopID) {
    return find_if(mAccesses, [LoopID](auto &Data) {
      return Data.template get<Loop>() == LoopID;
    });
  }

  auto find(ObjectID LoopID) const {
    return find_if(mAccesses, [LoopID](auto &Data) {
      return Data.template get<Loop>() == LoopID;
    });
  }

  void erase(ObjectID LoopID) {
    auto I = find(LoopID);
    if (I != end())
      mAccesses.erase(I);
  }

  auto numberOfMultipliers(ObjectID LoopID) const {
    auto I = find(LoopID);
    return I != mAccesses.end() ? I->get<RemoteData>().size() : 0;
  }

  bool hasAffineAccess(ObjectID LoopID) const {
    auto I = find(LoopID);
    return I != mAccesses.end() ? !I->get<RemoteData>().empty() : false;
  }

  auto begin(ObjectID LoopID) {
    assert(hasAffineAccess(LoopID) &&
           "There are no subscript expressions which refer the base induction "
           "variable of a specified loop!");
    return I->get<RemoteData>().begin();
  }

  auto end(ObjectID LoopID) {
    assert(hasAffineAccess(LoopID) &&
           "There are no subscript expressions which refer the base induction "
           "variable of a specified loop!");
    return I->get<RemoteData>().end();
  }

  auto begin(ObjectID LoopID) const {
    assert(hasAffineAccess(LoopID) &&
           "There are no subscript expressions which refer the base induction "
           "variable of a specified loop!");
    return I->get<RemoteData>().begin();
  }

  auto end(ObjectID LoopID) const {
    assert(hasAffineAccess(LoopID) &&
           "There are no subscript expressions which refer the base induction "
           "variable of a specified loop!");
    return I->get<RemoteData>().end();
  }

  bool hasConstantAccess() const noexcept {
    return mConstantAccesses.empty();
  }

  void setUnknownAccess(bool IsWrite) noexcept {
    mHasUnknownAccess = true;
    mHasWriteConflict |= IsWrite;
  }
  bool hasUnknownAccess() const noexcept { return mHasUnknownAccess; }
  bool hasWriteConflict() const noexcept { return mHasWriteConflict; }

  /// Return false if there are unknown subscripts which does not refer
  /// base induction variables of specified loops.
  template<typename LoopList>
  bool hasUnknownAccess(LoopList &&Loops) const {
    if (hasUnknownAccess())
      return true;
    for (auto *ID : loops()) {
      auto I = Loops.begin(), EI = Loops.end();
      for (; I != EI; ++I)
        if (*I == ID)
          break;
      if (I == EI)
        return true;
    }
    return false;
  }

  template<typename LoopItrT>
  bool hasUnknownAccess(LoopItrT I, LoopItrT EI) const {
    return hasUnknownAccess(make_range(I, EI));
  }

  std::pair<APInt, APInt> getBounds() const { return mBounds; }

  void addInductionForLoop(ObjectID L) { mLoops.insert(L); }
  bool hasAccessToInduction(ObjectID L) const { return mLoops.count(L); }

  auto loop_begin() { return mLoops.begin(); }
  auto loop_end() { return mLoops.end(); }

  auto loops() { return make_range(loop_begin(), loop_end()); }

  auto loop_begin() const { return mLoops.begin(); }
  auto loop_end() const { return mLoops.end(); }

  auto loops() const { return make_range(loop_begin(), loop_end()); }

  auto loop_size() const { return mLoops.size(); }
  auto loop_empty() const { return mLoops.empty(); }

  MILPColumnT getDimensionID() const noexcept { return mColumn; }
  unsigned getDimension() const noexcept { return mDimension; }

  const auto &getWrite() const noexcept { return mWrite; }

private:
  MILPColumnT mColumn;
  unsigned mDimension;

  /// Dimension bounds which enclose all accesses in the analyzed region.
  std::pair<APInt, APInt> mBounds = {APInt::getSignedMaxValue(BitWidth),
                                     APInt::getSignedMinValue(BitWidth)};

  /// For each loop this stores affine accesses A * I + B and describes data
  /// transfers which are implied by corresponding accesses.
  ///
  /// Actually, for each loop this stores a map from A to range of B. For
  /// example, accesses i + 1 and i - 1 produce map from 1 to [-1, 1].
  SmallVector<
      bcl::tagged_pair<
          bcl::tagged<ObjectID, Loop>,
          bcl::tagged<std::map<APInt, RemoteData, APIntLess>, RemoteData>>,
      3>
      mAccesses;


  /// This stores constant subscripts.
  SmallSet<APInt, 4, APIntLess> mConstantAccesses;

  /// Loops with base induction variables which are mentioned in subscripts.
  SmallSet<ObjectID, 3> mLoops;

  bool mHasUnknownAccess = false;
  bool mHasWriteConflict = false;


  struct AffineAccess {
    ObjectID LoopID;
    APInt Multiplier;
    APInt Constant;
  };
  Optional<AffineAccess> mWrite;
};

template<unsigned BitWidth = 64>
class ParallelNestAccessBase {
  using LoopList =
      SmallVector<bcl::tagged_tuple<bcl::tagged<ObjectID, Loop>,
                                    bcl::tagged<APInt, trait::Induction>,
                                    bcl::tagged<DebugLoc, DebugLoc>,
                                    bcl::tagged<MILPColumnT, MILPColumnT>>,
                  4>;
public:
  using DimensionAccess = DimensionAccessBase<64>;

private:
  using LoopAccessMap = DenseMap<
      DIMemory *, SmallVector<DimensionAccess, 4>, DenseMapInfo<DIMemory *>,
      TaggedDenseMapPair<
          bcl::tagged<DIMemory *, DIMemory>,
          bcl::tagged<SmallVector<DimensionAccess, 4>, DimensionAccess>>>;

public:
  static constexpr unsigned getBitWidth() noexcept {
    return BitWidth;
  }

  explicit ParallelNestAccessBase(Function &F) : mFunc(&F) {}

  Function &getFunction() noexcept { return *mFunc; }
  const Function &getFunction() const noexcept { return *mFunc; }

  void push_back() { mLoops.emplace_back(); }
  void pop_back() { mLoops.pop_back(); }

  bool empty() const { return mLoops.empty(); }
  auto size() const { return mLoops.size(); }

  auto &back() { return mLoops.back(); }
  const auto &back() const { return mLoops.back(); }

  auto &front() { return mLoops.front(); }
  const auto &front() const { return mLoops.front(); }

  auto begin() { return mLoops.begin(); }
  auto end() { return mLoops.end(); }

  auto begin() const { return mLoops.begin(); }
  auto end() const { return mLoops.end(); }

  auto &operator[](unsigned LoopIdx) { return mLoops[LoopIdx]; }

  auto find(ObjectID LoopID) {
    return find_if(mLoops,
                   [LoopID](auto &L) { return L.get<Loop>() == LoopID; });
  }

  auto find(ObjectID LoopID) const {
    return find_if(mLoops,
                   [LoopID](auto &L) { return L.get<Loop>() == LoopID; });
  }

  auto find_array(DIMemory *DIM) {
    return mArrays.find(DIM);
  }

  auto find_array(DIMemory *DIM) const {
    return mArrays.find(DIM);
  }

  DimensionAccessBase<BitWidth> *find_array(DIMemory *DIM, unsigned Dimension) {
    return const_cast<DimensionAccessBase<BitWidth> *>(
        static_cast<ParallelNestAccess<BitWidth> *>(this));
  }

  const DimensionAccessBase<BitWidth> *find_array(DIMemory *DIM,
                                                  unsigned Dimension) const {
    auto I = find_array(DIM);
    if (I == array_end())
      return nullptr;
    auto DimItr = find_if(I->get<DimensionAccess>(), [Dimension](auto &Access) {
      return Access.getDimension() == Dimension;
    });
    return DimItr == I->get<DimensionAccess>().end() ? nullptr : &*DimItr;
  }

  auto array_insert(DIMemory *DIM) { return mAccesses.try_emplace(DIM); }

  auto array_begin() { return mAccesses.begin(); }
  auto array_end() { return mAccesses.end(); }

  auto array_begin() const { return mAccesses.begin(); }
  auto array_end() const { return mAccesses.end(); }

  auto arrays() { return make_range(array_begin(), array_end()); }
  auto arrays() const { return make_range(array_begin(), array_end()); }

  auto array_size() const { return mAccesses.size(); }
  bool array_empty() const { return mAccesses.empty(); }

  const APInt &getOuterCount() const noexcept { return mOuterCount; }
  void setOuterCount(const APInt &OuterCount) { mOuterCount = OuterCount; }

private:
  APInt mOuterCount;
  Function *mFunc = nullptr;
  LoopList mLoops;
  LoopAccessMap mAccesses;
};

using ParallelNestAccess = ParallelNestAccessBase<64>;
using DimensionAccess = ParallelNestAccess::DimensionAccess;

using ClangParallelProvider =
    FunctionPassAAProvider<AnalysisSocketImmutableWrapper, LoopInfoWrapperPass,
                           ParallelLoopPass, MemoryMatcherImmutableWrapper,
                           TransformationEnginePass, DIEstimateMemoryPass,
                           CanonicalLoopPass, DFRegionInfoPass,
                           ClangPerfectLoopPass>;

class ClangDVMHParallelization : public ModulePass, bcl::Uncopyable {
public:
  static char ID;

  ClangDVMHParallelization() : ModulePass(ID) {
    initializeClangDVMHParallelizationPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override {
    mRegions.clear();
    mTfmCtx = nullptr;
    mGlobalOpts = nullptr;
    mMemoryMatcher = nullptr;
    mGlobalsAA = nullptr;
    mSocketInfo = nullptr;
    mArrayAccesses = nullptr;
    mParallelNests.clear();
    *FreeColumn = 1;
    mWeights = trait::Weights{};
  }

private:
  void initializeProviderOnClient(llvm::Module &M);

  APInt computeTripCount(Function &F, ObjectID &LoopID) {
    auto &Socket = mSocketInfo->getActive()->second;
    auto RM = Socket.getAnalysis<AnalysisClientServerMatcherWrapper>();
    auto RF =
        Socket.getAnalysis<DIEstimateMemoryPass, DIDependencyAnalysisPass>(F);
    auto &ClientToServer = **RM->value<AnalysisClientServerMatcherWrapper *>();
    auto ServerLoopID = cast<MDNode>(*ClientToServer.getMappedMD(LoopID));
    auto &DIDepInfo =
        RF->value<DIDependencyAnalysisPass *>()->getDependencies();
    auto DIDepSet = DIDepInfo[ServerLoopID];
    for (auto &TS : DIDepSet) {
      if (!TS.is<trait::Induction>())
        continue;
      for (auto &T : TS)
        if (auto I = T->get<trait::Induction>())
          if (I->getStart() && I->getEnd() && I->getStep())
            return ((*I->getEnd() - *I->getStart()) / *I->getStep())
                .sextOrSelf(ParallelNestAccess::getBitWidth());
    }
    return APInt::getNullValue(ParallelNestAccess::getBitWidth());
  }

  template <typename ItrT>
  void findParallelNests(ItrT I, ItrT EI, const APInt &OuterCount,
      const ClangParallelProvider &Provider) {
    assert(!mParallelNests.empty() && mParallelNests.back().empty() &&
           "Access storage is not available!");
    if (I == EI)
      return;
    for (; I != EI; ++I) {
      findParallelNests(**I, OuterCount, Provider, mParallelNests.back());
      if (!mParallelNests.back().empty())
        mParallelNests.emplace_back(mParallelNests.back().getFunction());
    }
  }

  void findParallelNests(Loop &L, const APInt &OuterCount,
      const ClangParallelProvider &Provider, ParallelNestAccess &Nest);

  void printParallelNests(raw_ostream &OS) const;

  tsar::TransformationContext *mTfmCtx = nullptr;
  const tsar::GlobalOptions *mGlobalOpts = nullptr;
  tsar::MemoryMatchInfo *mMemoryMatcher = nullptr;
  GlobalsAAResult * mGlobalsAA = nullptr;
  tsar::AnalysisSocketInfo *mSocketInfo = nullptr;
  tsar::DIMemoryEnvironment *mDIMEnv = nullptr;
  tsar::DIArrayAccessInfo *mArrayAccesses = nullptr;
  SmallVector<const tsar::OptimizationRegion *, 4> mRegions;
  std::vector<ParallelNestAccess> mParallelNests;
  trait::Weights mWeights;
};

class ClangParallelizationInfo final : public tsar::PassGroupInfo {
  void addBeforePass(legacy::PassManager &Passes) const {
    addImmutableAliasAnalysis(Passes);
    addInitialTransformations(Passes);
    Passes.add(createAnalysisSocketImmutableStorage());
    Passes.add(createDIMemoryTraitPoolStorage());
    Passes.add(createDIMemoryEnvironmentStorage());
    Passes.add(createDIEstimateMemoryPass());
    Passes.add(createDIMemoryAnalysisServer());
    Passes.add(createAnalysisWaitServerPass());
    Passes.add(createMemoryMatcherPass());
    Passes.add(createAnalysisWaitServerPass());
  }

  void addAfterPass(legacy::PassManager &Passes) const {
    Passes.add(createAnalysisReleaseServerPass());
    Passes.add(createAnalysisCloseConnectionPass());
  }
};
}

void ClangDVMHParallelization::initializeProviderOnClient(llvm::Module &M) {
  ClangParallelProvider::initialize<GlobalOptionsImmutableWrapper>(
      [this](GlobalOptionsImmutableWrapper &Wrapper) {
        Wrapper.setOptions(mGlobalOpts);
      });
  ClangParallelProvider::initialize<AnalysisSocketImmutableWrapper>(
      [this](AnalysisSocketImmutableWrapper &Wrapper) {
        Wrapper.set(*mSocketInfo);
      });
  ClangParallelProvider::initialize<TransformationEnginePass>(
      [this, &M](TransformationEnginePass &Wrapper) {
        Wrapper.setContext(M, mTfmCtx);
      });
  ClangParallelProvider::initialize<MemoryMatcherImmutableWrapper>(
      [this](MemoryMatcherImmutableWrapper &Wrapper) {
        Wrapper.set(*mMemoryMatcher);
      });
  ClangParallelProvider::initialize<
      GlobalsAAResultImmutableWrapper>(
      [this](GlobalsAAResultImmutableWrapper &Wrapper) {
        Wrapper.set(*mGlobalsAA);
      });
  ClangParallelProvider::initialize<DIMemoryEnvironmentWrapper>(
      [this](DIMemoryEnvironmentWrapper &Wrapper) {
        Wrapper.set(*mDIMEnv);
      });
}

#ifdef LLVM_DEBUG
inline static void ignoreLoopLog(const Loop &L, StringRef Message) {
  dbgs() << "[DVMH PARALLEL]: ignore loop at ";
  L.getStartLoc().print(dbgs());
  dbgs() << ": " << Message << "\n";
}
#endif

void ClangDVMHParallelization::findParallelNests(Loop &L,
    const APInt &OuterCount, const ClangParallelProvider &Provider,
    ParallelNestAccess &Nest) {
  APInt DefaultTripCount{
      ParallelNestAccess::getBitWidth(),
      mWeights[trait::Weights::Software][trait::Software::TripCount]};
  auto LoopID = L.getLoopID();
  if (!LoopID) {
    LLVM_DEBUG(ignoreLoopLog(L, "the loop does not have identifier"));
    if (Nest.empty())
      findParallelNests(L.begin(), L.end(), OuterCount * DefaultTripCount,
                        Provider);
    return;
  }
  auto &F = *L.getHeader()->getParent();
  auto TripCount = computeTripCount(F, LoopID);
  if (TripCount.isNullValue()) {
    LLVM_DEBUG(ignoreLoopLog(L, "unable to compute trip count"));
    if (Nest.empty())
      findParallelNests(L.begin(), L.end(), OuterCount * DefaultTripCount,
                        Provider);
    return;
  }
  if (!mRegions.empty() && std::none_of(mRegions.begin(), mRegions.end(),
                                        [&L](const OptimizationRegion *R) {
                                          return R->contain(L);
                                        })) {
    LLVM_DEBUG(
        ignoreLoopLog(L, "the loop is not located in any optimization region"));
    if (Nest.empty())
      findParallelNests(L.begin(), L.end(), OuterCount * TripCount, Provider);
    return;
  }
  auto &PL = Provider.get<ParallelLoopPass>().getParallelLoopInfo();
  if (!PL.count(&L)) {
    LLVM_DEBUG(ignoreLoopLog(L, "the loop is not parallelizable"));
    if (Nest.empty())
      findParallelNests(L.begin(), L.end(), OuterCount * TripCount, Provider);
    return;
  }
  auto &CI = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
  auto &RI = Provider.get<DFRegionInfoPass>().getRegionInfo();
  auto DFL = cast<DFLoop>(RI.getRegionFor(&L));
  auto CanonicalItr = CI.find_as(DFL);
  if (CanonicalItr == CI.end() || !(**CanonicalItr).isCanonical()) {
    LLVM_DEBUG(ignoreLoopLog(L, "the loop does not have canonical loop form"));
    if (Nest.empty())
      findParallelNests(L.begin(), L.end(), OuterCount * TripCount, Provider);
    return;
  }
  Nest.push_back();
  Nest.back().get<Loop>() = LoopID;
  Nest.back().get<DebugLoc>() = L.getStartLoc();
  Nest.back().get<trait::Induction>() = TripCount;
  SmallPtrSet<DIMemory *, 8> HasUnknownAccess;
  for (auto &Access : mArrayAccesses->scope_accesses(LoopID)) {
    // TODO (kaniandr@gmail.com): ignore privatizable arrays
    // TODO (kaniandr@gmail.com): check that array is distributable in the
    // optimization region, otherwise it should be copied, initially, ignore
    // loops which access such arrays.
    if (Access.empty())
      if (!Access.isReadOnly()) {
        LLVM_DEBUG(ignoreLoopLog(L, "the loop has unknown write accesses"));
        // TODO (kaniandr@gmail.com): try to remember an influence of the
        // loop on data distribution with lower weights.
        Nest.pop_back();
        for (auto &ArrayAccess : Nest.arrays())
          for (auto &DimAccess : ArrayAccess.get<DimensionAccess>())
            DimAccess.erase(LoopID);
        if (Nest.empty())
          findParallelNests(L.begin(), L.end(), OuterCount * TripCount,
                            Provider);
        return;
      } else {
        HasUnknownAccess.insert(Access.getArray());
        continue;
      }
    auto ArrayInfo = Nest.array_insert(Access.getArray());
    if (ArrayInfo.second) {
      ArrayInfo.first->get<DimensionAccess>().reserve(Access.size());
      for (auto Dim : seq(0u, (unsigned)Access.size()))
        ArrayInfo.first->get<DimensionAccess>().emplace_back(Dim,
                                                             getFreeColumn());
    } else {
      unsigned PrevNumberOfDims =
          ArrayInfo.first->get<DimensionAccess>().size();
      unsigned CurrNumberOfDims = Access.size();
      if (PrevNumberOfDims != CurrNumberOfDims) {
        if (PrevNumberOfDims < CurrNumberOfDims) {
          ArrayInfo.first->get<DimensionAccess>().reserve(CurrNumberOfDims);
          for (auto Dim : seq(PrevNumberOfDims, CurrNumberOfDims))
            ArrayInfo.first->get<DimensionAccess>().emplace_back(
                Dim, getFreeColumn());
          std::swap(PrevNumberOfDims, CurrNumberOfDims);
        }
        for (auto I = CurrNumberOfDims; I < PrevNumberOfDims; ++I)
          ArrayInfo.first->get<DimensionAccess>()[I].setUnknownAccess(
              !Access.isReadOnly());
      }
    }
    for (unsigned DimIdx = 0, DimIdxE = Access.size(); DimIdx < DimIdxE;
         ++DimIdx) {
      auto &DimAccess = ArrayInfo.first->get<DimensionAccess>()[DimIdx];
      if (auto *Affine = dyn_cast_or_null<DIAffineSubscript>(Access[DimIdx])) {
        if (Affine->getNumberOfMonoms() == 0) {
          DimAccess.add(Affine->getConstant(), !Access.isReadOnly());
        } else {
          unsigned MonomIdxE = Affine->getNumberOfMonoms();
          auto MonomIdx = MonomIdxE;
          for (auto I = 0u; I < MonomIdxE; ++I) {
            if (Affine->getMonom(I).Column == LoopID)
              MonomIdx = I;
            DimAccess.addInductionForLoop(Affine->getMonom(I).Column);
          }
          if (MonomIdx < MonomIdxE) {
            if (MonomIdxE == 1) {
              DimAccess.add(Affine->getMonom(MonomIdx).Value,
                            Affine->getConstant(), TripCount, LoopID,
                            !Access.isReadOnly());
            } else {
              DimAccess.setUnknownAccess(!Access.isReadOnly());
            }
          }
        }
      } else {
        DimAccess.setUnknownAccess(!Access.isReadOnly());
        // TODO (kaniandr@gmail.com): check whether subscript is loop
        // invariant, may be it is possible to transfer only one element
        // instead of the whole dimension
      }
      if (HasUnknownAccess.count(Access.getArray()))
        DimAccess.setUnknownAccess(false);
    }
  }
  Nest.back().get<MILPColumnT>() = getFreeColumn();
  Nest.setOuterCount(OuterCount);
  auto &PI = Provider.get<ClangPerfectLoopPass>().getPerfectLoopInfo();
  if (!L.empty() && PI.count(DFL))
    findParallelNests(**L.begin(), OuterCount, Provider, Nest);
}

bool ClangDVMHParallelization::runOnModule(llvm::Module &M) {
  releaseMemory();
  mTfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!mTfmCtx || !mTfmCtx->hasInstance()) {
    M.getContext().emitError("cannot transform sources"
                             ": transformation context is not available");
    return false;
  }
  mSocketInfo = &getAnalysis<AnalysisSocketImmutableWrapper>().get();
  mGlobalOpts = &getAnalysis<GlobalOptionsImmutableWrapper>().getOptions();
  mMemoryMatcher = &getAnalysis<MemoryMatcherImmutableWrapper>().get();
  mGlobalsAA = &getAnalysis<GlobalsAAWrapperPass>().getResult();
  mDIMEnv = &getAnalysis<DIMemoryEnvironmentWrapper>().get();
  mArrayAccesses = getAnalysis<DIArrayAccessWrapper>().getAccessInfo();
  if (!mArrayAccesses) {
    M.getContext().emitError("cannot collect array accesses");
    return false;
  }
  initializeProviderOnClient(M);
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
  auto &Socket = mSocketInfo->getActive()->second;
  auto RM = Socket.getAnalysis<AnalysisClientServerMatcherWrapper>();
  for (auto &F : M) {
    if (F.isDeclaration())
      continue;
    mParallelNests.emplace_back(F);
    auto &Provider = getAnalysis<ClangParallelProvider>(F);
    auto &LI = Provider.get<LoopInfoWrapperPass>().getLoopInfo();
    findParallelNests(LI.begin(), LI.end(),
                      APInt(ParallelNestAccess::getBitWidth(), 1), Provider);
    assert(mParallelNests.back().empty() && "Invariant broken!");
    mParallelNests.pop_back();
  }
  LLVM_DEBUG(dbgs() << "[DVMH PARALLEL]: number of collected parallel nests "
                    << mParallelNests.size() << "\n");
  LLVM_DEBUG(printParallelNests(dbgs()));
  auto NumberOfColumns = (*FreeColumn) - 1;
  LLVM_DEBUG(dbgs() << "[DVMH PARALLEL]: maximum number of columns in solver "
                    << NumberOfColumns << "\n");
  milp::BinomialSystem<MILPColumnT, int64_t, 1, 1, 1> LinearSystem;
  lprec *LP = nullptr;
  if (!(LP = make_lp(0, NumberOfColumns))) {
    M.getContext().emitError("unable to create linear programming model");
    return false;
  }
  set_add_rowmode(LP, TRUE);
  auto emitError = [&M, LP](const Twine &Msg) {
    M.getContext().emitError(Msg);
    delete_lp(LP);
  };
  for (auto &Nest : mParallelNests) {
    auto DWLang = getLanguage(Nest.getFunction());
    if (!DWLang) {
      emitError("unable to determine the source language for '" +
                Nest.getFunction().getName() + "' function");
      return false;
    }
    for (auto &L : Nest) {
      SmallString<32> ColumnName;
      raw_svector_ostream OS(ColumnName);
      OS << "L.";
      if (L.get<DebugLoc>())
        tsar::print(OS, L.get<DebugLoc>(), true);
      else
        OS << L.get<MILPColumnT>();
      set_col_name(LP, L.get<MILPColumnT>(),
                   const_cast<char *>(ColumnName.c_str()));
      if (!set_int(LP, L.get<MILPColumnT>(), TRUE) ||
          !set_bounds(LP, L.get<MILPColumnT>(), 0,
                      L.get<trait::Induction>().getSExtValue())) {
        emitError("unable to set bounds for " + OS.str());
        return false;
      }
    }
    for (auto &Array : Nest.arrays()) {
      SmallString<32> DimensionName;
      raw_svector_ostream DimensionNameOS(DimensionName);
      printDILocationSource(*DWLang, *Array.get<DIMemory>(), DimensionNameOS);
      auto DimensionNameSize = DimensionName.size();
      for (auto &DimAccess : Array.get<DimensionAccess>()) {
        DimensionName.resize(DimensionNameSize);
        DimensionNameOS << "." << DimAccess.getDimension();
        set_col_name(LP, DimAccess.getDimensionID(),
          const_cast<char *>(DimensionName.c_str()));
        if (!set_int(LP, DimAccess.getDimensionID(), TRUE) ||
            !set_bounds(LP, DimAccess.getDimensionID(),
                        DimAccess.getBounds().first.getSExtValue(),
                        DimAccess.getBounds().second.getSExtValue())) {
          emitError("unable to set bounds for " + DimensionName);
          return false;
        }
        for (auto &LpAccess: DimAccess)
          for (auto &Access : LpAccess.get<RemoteData>()) {
            auto LpInfoItr = Nest.find(LpAccess.get<Loop>());
            SmallString<64> CurrentName;
            raw_svector_ostream CurrentNameOS(CurrentName);
            CurrentNameOS << DimensionName << "." << Access.first << "."
                          << get_col_name(LP, LpInfoItr->get<MILPColumnT>());
            auto initShadow = [&emitError, LP, &DimensionName, &Nest, &LpAccess,
                               &Access](auto Shadow, const Twine &Prefix) {
              SmallString<64> Name;
              Prefix.toVector(Name);
              set_col_name(LP, Shadow.Column, const_cast<char *>(Name.c_str()));
              if (!set_int(LP, Shadow.Column, TRUE)) {
                emitError("unable to set type for " + Name);
                return false;
              }
              Name += ".F";
              set_col_name(LP, Shadow.ChoiceColumn,
                           const_cast<char *>(Name.c_str()));
              if (!set_binary(LP, Shadow.ChoiceColumn, TRUE)) {
                emitError("unable to set type for" + Name);
                return false;
              }
            };
            if (!initShadow(Access.second.Left, "SL." + CurrentName))
              return false;
            if (!initShadow(Access.second.Right, "SR." + CurrentName))
              return false;
            if (!addConstraintex(LP,
                                 {Access.second.Left.ChoiceColumn,
                                  Access.second.Right.ChoiceColumn},
                                 {1, 1}, EQ, 1)) {
              emitError(
                  "unable to set constraints for the shadow choice flags");
              return false;
            }
            set_col_name(
                LP, Access.second.Remote.Column,
                const_cast<char *>(("R." + CurrentName).str().c_str()));
            if (!set_binary(LP, Access.second.Remote.Column, TRUE)) {
              emitError("unable to set type for remote flag");
              return false;
            }
            auto AlwaysFeasibleGuard =
                DimAccess.getBounds().second -
                (APIntOps::smin(Access.first * 0,
                                Access.first *
                                    LpInfoItr->get<trait::Induction>()) +
                 DimAccess.getBounds().first);
            auto Constant = AlwaysFeasibleGuard + DimAccess.getBounds().first;
            if (!addConstraintex(
                    LP,
                    {DimAccess.getDimensionID(), LpInfoItr->get<MILPColumnT>(),
                     Access.second.Left.Column, Access.second.Left.ChoiceColumn,
                     Access.second.Remote.Column},
                    {1, -1, -1, (double)(-AlwaysFeasibleGuard).getSExtValue(),
                     (double)(-AlwaysFeasibleGuard).getSExtValue()},
                    LE, Constant.getSExtValue())) {
              emitError("unable to set 'less' constraint of the left shadow");
              return false;
            }
          }
      }
    }
  }
  set_add_rowmode(LP, FALSE);
  set_minim(LP);
  LLVM_DEBUG(write_LP(LP, stderr));
  return false;
}

void ClangDVMHParallelization::printParallelNests(raw_ostream &OS) const {
  for (auto &Nest : mParallelNests) {
    dbgs() << "Size of nest: " << Nest.size() << "\n";
    dbgs() << "  Outer loops trip count: " << Nest.getOuterCount() << "\n";
    for (auto &Array : Nest.arrays()) {
      dbgs() << "  Accesses to ";
      printDILocationSource(dwarf::DW_LANG_C, *Array.get<DIMemory>(), dbgs());
      dbgs() << "\n";
      for (auto &DimAccess : Array.get<DimensionAccess>()) {
        dbgs() << "    Dimension " << DimAccess.getDimension() << "\n";
        dbgs() << "      Dimension ID " << DimAccess.getDimensionID() << "\n";
        dbgs() << "      Unknown accesses: "
               << (DimAccess.hasUnknownAccess() ? "true" : "false") << "\n";
        dbgs() << "      Loop unknown accesses: "
               << (DimAccess.hasUnknownAccess(map_range(
                       Nest, [](auto &V) { return V.template get<Loop>(); }))
                       ? "true"
                       : "false")
               << "\n";
        dbgs() << "      Write conflicts: "
               << (DimAccess.hasWriteConflict() ? "true" : "false") << "\n";
        dbgs() << "      Constant accesses: "
               << (DimAccess.hasConstantAccess() ? "true" : "false") << "\n";
        dbgs() << "      Number of multipliers:\n";
        for (auto &L : Nest)
          dbgs() << "        LoopID " << L.get<MILPColumnT>() << ": "
                 << DimAccess.numberOfMultipliers(L.get<Loop>()) << "\n";
        if (DimAccess.empty())
          continue;
        dbgs() << "      Accessed Bounds: [" << DimAccess.getBounds().first
               << ", " << DimAccess.getBounds().second << "]\n";
        if (DimAccess.getWrite()) {
          dbgs() << "      Write access: " << DimAccess.getWrite()->Multiplier
                 << " => " << DimAccess.getWrite()->Constant << " loop ID"
                 << Nest.find(DimAccess.getWrite()->LoopID)->get<MILPColumnT>()
                 << "\n";
        }
        dbgs() << "      Accesses:\n";
        for (auto &LpAccess : DimAccess) {
          dbgs() << "      Loop ID "
                 << Nest.find(LpAccess.get<Loop>())->get<MILPColumnT>() << "\n";
          for (auto &Access : LpAccess.get<RemoteData>())
            dbgs() << "      " << Access.first << " => ["
                   << Access.second.Shadows.min() << ", "
                   << Access.second.Shadows.max() << "]\n";
        }
      }
    }
  }
}

void ClangDVMHParallelization::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<ClangParallelProvider>();
  AU.addRequired<AnalysisSocketImmutableWrapper>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.addRequired<ClangRegionCollector>();
  AU.addRequired<DIArrayAccessWrapper>();
  AU.addRequired<GlobalOptionsImmutableWrapper>();
  AU.addRequired<GlobalsAAWrapperPass>();
  AU.addRequired<DIMemoryEnvironmentWrapper>();
  AU.setPreservesAll();
}

ModulePass *llvm::createClangDVMHParallelization() {
  return new ClangDVMHParallelization;
}

INITIALIZE_PROVIDER(ClangParallelProvider, "clang-dvmh-parallel-provider",
                    "DVMH-based Parallelization (Clang, Provider)")

char ClangDVMHParallelization::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(ClangDVMHParallelization, "clang-dvmh-parallel",
                               "DVMH-based Parallelization (Clang)", false,
                               false,
                               TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_IN_GROUP_INFO(ClangParallelizationInfo)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(GlobalOptionsImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(ClangRegionCollector)
INITIALIZE_PASS_DEPENDENCY(DIArrayAccessWrapper)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalDefinedMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(GlobalLiveMemoryWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryEnvironmentWrapper)
INITIALIZE_PASS_DEPENDENCY(DIMemoryTraitPoolWrapper)
INITIALIZE_PASS_DEPENDENCY(ParallelLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangParallelProvider)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PASS_IN_GROUP_END(ClangDVMHParallelization, "clang-dvmh-parallel",
                             "DVMH-based Parallelization (Clang)", false, false,
                             TransformationQueryManager::getPassRegistry())
