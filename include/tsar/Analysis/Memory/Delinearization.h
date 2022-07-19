//===- Delinearization.h -- Delinearization Engine --------------*- C++ -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
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
// This file allows to perform metadata-based delinearization of array accesses.
//
//===----------------------------------------------------------------------===/

#ifndef TSAR_DELINIARIZATION_H
#define TSAR_DELINIARIZATION_H

#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/PointerIntPair.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>
#include <vector>

namespace llvm {
class DataLayout;
class Instruction;
class Function;
class Value;
class DominatorTree;
class TargetLibraryInfo;
}

namespace tsar {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// Delinearized array.
class Array {
  enum Flags : uint8_t {
    DefaultFlags = 0u,
    HasRangeRef = 1u << 0,
    IsDelinearized = 1u << 1,
    HasMetadata = 1u << 2,
    LLVM_MARK_AS_BITMASK_ENUM(HasMetadata)
  };
public:
  using ExprList = llvm::SmallVector<const llvm::SCEV *, 4>;
  using TypeList = llvm::SmallVector<const llvm::Type *, 4>;

  /// Map from linearized index of array to its delinearized representation.
  struct Range {
    enum Flags : uint8_t {
      DefaultFlags = 0u,
      /// Set if this class describes a single element of the array.
      IsElement = 1u << 0,
      /// Set if reference to this element has been successfully delinearized.
      /// In this case the list of subscripts contains delinearized subscript
      /// for each dimension. For A[I][J], subscripts will be I and J.
      /// If delinearization is not possible the list of subscripts contains
      /// original subscripts (A[I][J] ~ A + I*M + J): I*M and J.
      IsDelinearized = 1u << 1,
      /// Set if some extra zero subscripts must be added for delinearization.
      /// In some cases zero subscript is dropping out by optimization passes.
      /// So, we try to recover these zero subscripts.
      NeedExtraZero = 1u << 2,
      /// Set if delinearization ignores some beginning GEPs.
      IgnoreGEP = 1u << 3,
      /// Set if GEPs are not used to calculate address of the sub-range
      /// beginning. For example, if int to pointer conversion is used.
      NoGEP = 1u << 4,
      LLVM_MARK_AS_BITMASK_ENUM(NoGEP)
    };

    /// Pointer to the beginning of array sub-range.
    llvm::Value *Ptr;

    /// List of subscript expressions which allow to access this element.
    ///
    /// This is representation of offset (`Ptr-ArrayPtr`) after delinearization.
    ExprList Subscripts;

    /// List of indexed GEP types, each type is a result of an application of
    // a corresponding subscript.
    TypeList Types;

    /// Creates element referenced with a specified pointer. Initial this
    /// element is not delinearized yet.
    explicit Range(llvm::Value *Ptr) : Ptr(Ptr) {
      assert(Ptr && "Pointer must not be null!");
    }

    /// Returns true if this range has been successfully delinearized and
    /// there is no properties which may lead to incorrect delinearization.
    bool isValid() const {
      return (Property & IsDelinearized) && !(Property & IgnoreGEP);
    }

    /// Returns true if this range describes a single element of the array.
    bool isElement() const { return Property & IsElement; }

    void setProperty(Flags F) { Property |= F; }
    void unsetProperty(Flags F) { Property &= ~F; }
    bool is(Flags F) const { return Property & F; }

  private:
    Flags Property = DefaultFlags;
  };

  using Ranges = std::vector<Range>;
  using iterator = Ranges::iterator;
  using const_iterator = Ranges::const_iterator;

  /// Creates new array representation for an array which starts at
  /// a specified address. If `IsAddress` is true then a specified base is
  /// an address of an array address (for example alloc),
  /// otherwise it is address of array.
  explicit Array(llvm::Value *BasePtr, bool IsAddressOfVariable) :
    mBasePtr(BasePtr, IsAddressOfVariable) {
    assert(BasePtr && "Pointer to the array beginning must not be null!");
  }

  /// Returns pointer to the array beginning.
  const llvm::Value * getBase() const { return mBasePtr.getPointer(); }

  /// Returns pointer to the array beginning.
  llvm::Value * getBase() { return mBasePtr.getPointer(); }

  /// Returns `true` if getBase() returns an address of a variable which stores
  /// address of array, otherwise getBase() returns an address of this array and
  /// this method returns false.
  bool isAddressOfVariable() const { return mBasePtr.getInt(); }

  iterator begin() { return mRanges.begin(); }
  iterator end() { return mRanges.end(); }

  const_iterator begin() const { return mRanges.begin(); }
  const_iterator end() const { return mRanges.end(); }

  std::size_t size() const { return mRanges.size(); }
  bool empty() const { return mRanges.empty(); }

  /// Add element which is referenced in a source code with
  /// a specified pointer.
  Range & addRange(llvm::Value *Ptr) {
    mRanges.emplace_back(Ptr);
    return mRanges.back();
  }

  /// Set number of dimensions.
  ///
  /// If the current number of dimensions size is less than count,
  /// additional dimensions are appended and their sizes are initialized
  /// with `nullptr` expressions.
  void setNumberOfDims(std::size_t Count) { mDims.resize(Count, nullptr); }

  /// Return number of dimensions.
  std::size_t getNumberOfDims() const { return mDims.size(); }

  /// Set size of a specified dimension.
  void setDimSize(std::size_t DimIdx, const llvm::SCEV *Expr) {
    assert(Expr && "Expression must not be null!");
    assert(DimIdx < getNumberOfDims() && "Dimension index is out of range!");
    mDims[DimIdx] = Expr;
  }

  /// Return size of a specified dimension.
  const llvm::SCEV * getDimSize(std::size_t DimIdx) const {
    assert(DimIdx < getNumberOfDims() && "Dimension index is out of range!");
    return mDims[DimIdx];
  }

  /// Return true if size of a specified dimension is known. If size is unknown
  /// it may be nullptr or llvm::SCEVCouldNotCompute.
  bool isKnownDimSize(std::size_t DimIdx) const {
    assert(DimIdx < getNumberOfDims() && "Dimension index is out of range!");
    return mDims[DimIdx] &&
      !llvm::isa<llvm::SCEVCouldNotCompute>(mDims[DimIdx]);
  }

  /// Return true if this array has been successfully delinearized. Note, that
  /// delinearization process may ignore pointers to some ranges of this array.
  bool isDelinearized() const noexcept { return mF & IsDelinearized; }
  void setDelinearized() noexcept { mF |= IsDelinearized; }

  /// Return true if there is an instruction which computes a reference to
  /// the beginning of an array subrange. The simplest example of such
  /// instruction is GEP which returns pointer to an element of this array.
  bool hasRangeRef() const noexcept { return mF & HasRangeRef; }
  void setRangeRef() noexcept { mF |= HasRangeRef; }

  /// Returns true if metadata is available.
  bool hasMetadata() const noexcept { return mF & HasMetadata; }
  void setMetadata() noexcept { mF |= HasMetadata; }

private:
  friend struct ArrayMapInfo;
  friend class DelinearizeInfo;

  using BaseTy = llvm::PointerIntPair<llvm::Value *, 1, bool>;


  Range * getRange(std::size_t Idx) {
    assert(Idx < mRanges.size() && "Index is out of range!");
    return mRanges.data() + Idx;
  }

  const Range * getRange(std::size_t Idx) const {
    assert(Idx < mRanges.size() && "Index is out of range!");
    return mRanges.data() + Idx;
  }

  BaseTy mBasePtr;
  ExprList mDims;
  std::vector<Range> mRanges;
  Flags mF = DefaultFlags;
};

/// Implementation of llvm::DenseMapInfo which uses base pointer to compute
/// hash for tsar::Array *.
struct ArrayMapInfo : public llvm::DenseMapInfo<Array *> {
  static inline unsigned getHashValue(const Array *Arr) {
    return DenseMapInfo<Array::BaseTy>::getHashValue(Arr->mBasePtr);
  }
  static inline unsigned getHashValue(Array::BaseTy Base) {
    return DenseMapInfo<Array::BaseTy>::getHashValue(Base);
  }
  static inline unsigned getHashValue(std::pair<llvm::Value *, bool> Base) {
    return DenseMapInfo<Array::BaseTy>::getHashValue(
      Array::BaseTy(Base.first, Base.second));
  }
  static inline unsigned getHashValue(
      std::pair<const llvm::Value *, bool> Base) {
    return DenseMapInfo<Array::BaseTy>::getHashValue(
      Array::BaseTy(const_cast<llvm::Value *>(Base.first), Base.second));
  }
  using DenseMapInfo<Array *>::isEqual;
  static inline bool isEqual(Array::BaseTy LHS , const Array *RHS) {
    return RHS != ArrayMapInfo::getEmptyKey() &&
      RHS != ArrayMapInfo::getTombstoneKey() &&
      llvm::DenseMapInfo<Array::BaseTy>::isEqual(LHS, RHS->mBasePtr);
  }
  static inline bool isEqual(
      std::pair<llvm::Value *, bool> LHS, const Array *RHS) {
    return isEqual(Array::BaseTy(LHS.first, LHS.second), RHS);
  }
  static inline bool isEqual(
      std::pair<const llvm::Value *, bool> LHS, const Array *RHS) {
    return isEqual(
      Array::BaseTy(const_cast<llvm::Value *>(LHS.first), LHS.second), RHS);
  }
};
}

namespace tsar {
/// Contains a list of delinearized arrays and a list of accessed elements of
/// these arrays in a function.
class DelinearizeInfo {
  struct ElementInfo :
    llvm::detail::DenseMapPair<llvm::Value *, std::pair<Array *, std::size_t>> {
    llvm::Value * getElementPtr() { return getFirst(); }
    const llvm::Value *  getElementPtr() const { return getFirst(); }
    Array * getArray() { return getSecond().first; }
    const Array * getArray() const { return getSecond().first; }
    std::size_t getElementIdx() const { return getSecond().second; }
  };
  using RangeMap =
    llvm::DenseMap<llvm::Value *, std::pair<Array *, std::size_t>,
      llvm::DenseMapInfo<llvm::Value *>, ElementInfo>;
public:
  using ArraySet = llvm::DenseSet<Array *, ArrayMapInfo>;

  DelinearizeInfo() = default;
  ~DelinearizeInfo() { clear(); }

  DelinearizeInfo(const DelinearizeInfo &) = delete;
  DelinearizeInfo & operator=(const DelinearizeInfo &) = delete;

  DelinearizeInfo(DelinearizeInfo &&) = default;
  DelinearizeInfo & operator=(DelinearizeInfo &&) = default;

  /// Returns delinearized representation of a specified range of an array.
  std::pair<Array *, Array::Range *>
      findRange(const llvm::Value *ElementPtr) {
    auto Tmp =
      static_cast<const DelinearizeInfo *>(this)->findRange(ElementPtr);
    return std::make_pair(
      const_cast<Array *>(Tmp.first), const_cast<Array::Range *>(Tmp.second));
  }

  /// Returns delinearized representation of a specified range of an array.
  std::pair<const Array *, const Array::Range *>
    findRange(const llvm::Value *ElementPtr) const;

  /// Returns an array which starts at a specified address.
  ///
  /// If IsAddressOfVariable is `true` then `BasePtr` is considered as an
  /// address of a variable which contains address of an array.
  /// For example, if `BasePtr` is `alloca` then IsAddressOfVariable must be
  /// set to true.
  Array * findArray(const llvm::Value *BasePtr, bool IsAddressofVariable) {
    return const_cast<Array *>(
      static_cast<const DelinearizeInfo *>(this)->findArray(
        BasePtr, IsAddressofVariable));
  }

  /// Returns an array which starts at a specified address.
  ///
  /// If IsAddressOfVariable is `true` then `BasePtr` is considered as an
  /// address of a variable which contains address of an array.
  /// For example, if `BasePtr` is `alloca` then IsAddressOfVariable must be
  /// set to true.
  const Array * findArray(const llvm::Value *BasePtr,
      bool IsAddressOfVariable) const {
    auto ResultItr = mArrays.find_as(
      std::make_pair(BasePtr, IsAddressOfVariable));
    return (ResultItr != mArrays.end()) ? *ResultItr : nullptr;
  }

  /// Returns an array which contains a specified pointer.
  const Array * findArray(const llvm::Value *Ptr,
    const llvm::DataLayout &DL) const;

  /// Returns an array which contains a specified pointer.
  Array * findArray(llvm::Value *Ptr, const llvm::DataLayout &DL) {
    return const_cast<Array *>(
      static_cast<const DelinearizeInfo *>(this)->findArray(Ptr, DL));
  }

  /// Returns list of all delinearized arrays.
  ArraySet & getArrays() noexcept { return mArrays; }

  /// Returns list of all delinearized arrays.
  const ArraySet & getArrays() const noexcept { return mArrays; }

  /// Update cache to enable GEP-based search of array.
  void updateRangeCache();

  /// Remove all available information.
  void clear() {
    for (auto *A : mArrays)
      delete A;
    mArrays.clear();
    mRanges.clear();
  }

private:
  ArraySet mArrays;
  RangeMap mRanges;
};
}

namespace llvm {
/// This per-function pass performs delinearization of array accesses.
class DelinearizationPass : public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;

  DelinearizationPass() : FunctionPass(ID) {
    initializeDelinearizationPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override { mDelinearizeInfo.clear(); }

  /// Print all found arrays and accessed ranges in JSON format. Note, that
  /// is some range is invalid or an array has not been successfully
  /// delinearized then JSON also contains description of this objects.
  void print(raw_ostream &OS, const Module *M) const override;

  const tsar::DelinearizeInfo & getDelinearizeInfo() const {
    return mDelinearizeInfo;
  }

private:
  /// Investigate metadata for a specified array to determine number of
  /// its dimensions.
  ///
  /// \post Reset number of dimensions if known and set sizes of known
  /// dimensions. Sizes of other dimensions are not initialized.
  void findArrayDimensionsFromDbgInfo(tsar::Array &ArrayInfo);

  /// Collect arrays accessed in a specified function.
  ///
  /// This function also collect all ranges on an array which are referenced in
  /// a specified function. If metadata are available this function tries to
  /// determine number of array dimensions and size of each dimension.
  void collectArrays(Function &F);

  /// Try to determine values of unknown dimension sizes.
  ///
  /// If number of dimensions is unknown this method uses accesses to elements
  /// of a specified arrays to determine number of dimensions. Accesses to
  /// elements are also analyzed to determine sizes of dimensions.
  /// For example,
  ///   float A[N][M][L][S];
  ///   A[I][J][K][T] = 5.0;
  /// in linear form is
  ///    A + I * M * L * S + J * L * S + K * S + T.
  /// (1) To determine size of L-dimension we analyze GEPs and extract terms:
  ///   I * M * L * S and J * L * S.
  /// (2) We find GCD for all extracted terms for all accesses:
  ///   L * S.
  /// (3) We divide GCD by a product of sizes of dimensions from the right hand
  /// side of the analyzed dimension:
  ///   L * S / S = L
  /// (4) The result will be a size of the analyzed dimension.
  void fillArrayDimensionsSizes(tsar::Array &ArrayInfo);

  /// Remove sizes of dimensions from subscript expressions and add extra zero
  /// subscripts if necessary.
  ///
  /// For example,
  ///   float A[N][N][N];
  ///   A[I][0][J] = 5.0;
  /// is represented in LLVM IR as
  ///   A + I * N * N + J
  /// After processing of GEPs we find two subscripts: I * N * N, J.
  /// (1) We should divide the first subscript by size of dimensions to obtain
  /// original subscript representation: I.
  /// (2) We could not divide the second subscript J by N
  /// (size of last dimension), so we add extra zero subscript between I and J.
  /// (3) The result will be an original representation of subscripts: I, 0, J.
  /// If some of subscripts can not be processed safely then the whole element
  /// remains unchanged.
  void cleanSubscripts(tsar::Array &CurrentArray);

  tsar::DelinearizeInfo mDelinearizeInfo;
  DominatorTree *mDT = nullptr;
  ScalarEvolution *mSE = nullptr;
  LoopInfo *mLI = nullptr;
  TargetLibraryInfo *mTLI = nullptr;
  bool mIsSafeTypeCast = true;
  Type *mIndexTy = nullptr;
};
}

#endif //TSAR_DELINIARIZATION_H
