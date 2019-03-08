#ifndef TSAR_ARRAY_SUBSCRIPT_DELINEARIZE_H
#define TSAR_ARRAY_SUBSCRIPT_DELINEARIZE_H

#include <llvm/Pass.h>
#include <llvm/IR/Function.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <utility>
#include <set>
#include <map>
#include "tsar_bimap.h"
#include "tsar_pass.h"
#include "tsar_utility.h"
#include <llvm/Analysis/ScalarEvolutionExpressions.h>

#include "tsar_pass.h"
#include "tsar_transformation.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/PassAnalysisSupport.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/IR/Type.h>


#include <vector>
#include <utility>

namespace llvm {
class Instruction;
class Function;
class Value;
class SCEV;
class ScalarEvolution;
class SCEVMulExpr;
}

namespace tsar {
/// Delinearized array.
class Array {
public:
  using ExprList = llvm::SmallVector<const llvm::SCEV *, 4>;

  /// Map from linearized index of array to its delinearized representation.
  /// Instruction does not access memory.
  struct Element {
    /// Pointer to an element of an array.
    llvm::Value *Ptr;

    /// List of subscript expressions which allow to access this element.
    ///
    /// This is representation of offset (`Ptr-ArrayPtr`) after delinearization.
    ExprList Subscripts;

    /// `True` if address of this element of an array has been successfully
    /// delinearized.
    bool IsValid = false;

    /// Creates element referenced with a specified pointer. Initial this
    /// element is not delineariaced yet.
    explicit Element(llvm::Value *Ptr) : Ptr(Ptr) {
      assert(Ptr && "Pointer must not be null!");
    }
  };

  using Elements = std::vector<Element>;
  using iterator = Elements::iterator;
  using const_iterator = Elements::const_iterator;

  /// Creates new array representation for an array which starts at
  /// a specified address.
  explicit Array(llvm::Value *BasePtr) :  mBasePtr(BasePtr) {
    assert(BasePtr && "Pointer to the array beginning must not be null!");
  }

  /// Returns pointer to the array beginning.
  const llvm::Value * getBase() const { return mBasePtr; }

  /// Returns pointer to the array beginning.
  llvm::Value * getBase() { return mBasePtr; }

  iterator begin() { return mElements.begin(); }
  iterator end() { return mElements.end(); }

  const_iterator begin() const { return mElements.begin(); }
  const_iterator end() const { return mElements.end(); }

  std::size_t size() const { return mElements.size(); }
  bool empty() const { return mElements.empty(); }

  /// Add element which is referenced in a source code with
  /// a specified pointer.
  Element & addElement(llvm::Value *Ptr) {
    mElements.emplace_back(Ptr);
    return mElements.back();
  }

  Element * getElement(std::size_t Idx) {
    assert(Idx < mElements.size() && "Index is out of range!");
    return mElements.data() + Idx;
  }

  const Element * getElement(std::size_t Idx) const {
    assert(Idx < mElements.size() && "Index is out of range!");
    return mElements.data() + Idx;
  }

  /// Set number of dimensions.
  ///
  /// If the current number of dimenstions size is less than count,
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

  /// Returns size of a specified dimension.
  const llvm::SCEV * getDimSize(std::size_t DimIdx) const {
    assert(DimIdx < getNumberOfDims() && "Dimension index is out of range!");
    return mDims[DimIdx];
  }

  bool isValid() { return !mDims.empty(); }

private:
  llvm::Value *mBasePtr;
  ExprList mDims;
  std::vector<Element> mElements;
};

std::pair<const llvm::SCEV *, const llvm::SCEV *> findCoefficientsInSCEV(
  const llvm::SCEV *Expr, llvm::ScalarEvolution &SE);
}

namespace llvm {
template<> struct DenseMapInfo<tsar::Array> {
  static unsigned getHashValue(const tsar::Array &Arr) {
    return DenseMapInfo<Value *>::getHashValue(Arr.getBase());
  }

  static bool isEqual(const tsar::Array &LHS, const tsar::Array &RHS) {
    return DenseMapInfo<Value *>::isEqual(LHS.getBase(), RHS.getBase());
  }

  static unsigned getHashValue(llvm::Value *Arr) {
    return DenseMapInfo<Value *>::getHashValue(Arr);
  }

  static bool isEqual(llvm::Value *LHS , const tsar::Array &RHS) {
    return DenseMapInfo<Value *>::isEqual(LHS, RHS.getBase());
  }

  static tsar::Array getEmptyKey() {
    return tsar::Array(DenseMapInfo<Value *>::getEmptyKey());
  }

  static tsar::Array getTombstoneKey() {
    return tsar::Array(DenseMapInfo<Value *>::getTombstoneKey());
  }
};
}

namespace tsar {
/// Contains a list of delinearized arrays and a list of accessed elements of
/// these arrays in a function.
class DelinearizeInfo : private bcl::Uncopyable {
  struct ElementInfo :
    llvm::detail::DenseMapPair<llvm::Value *, std::pair<Array *, std::size_t>> {
    llvm::Value * getElementPtr() { return getFirst(); }
    const llvm::Value *  getElementPtr() const { return getFirst(); }
    Array * getArray() { return getSecond().first; }
    const Array * getArray() const { return getSecond().first; }
    std::size_t getElementIdx() const { return getSecond().second; }
  };
  using ElementMap =
    llvm::DenseMap<llvm::Value *, std::pair<Array *, std::size_t>,
      llvm::DenseMapInfo<llvm::Value *>, ElementInfo>;
public:
  using ArraySet = llvm::DenseSet<Array>;

  /// Returns delinearized representation of a specified element of an array.
  std::pair<Array *, Array::Element *>
      findElement(const llvm::Value *ElementPtr) {
    auto Tmp =
      static_cast<const DelinearizeInfo *>(this)->findElement(ElementPtr);
    return std::make_pair(
      const_cast<Array *>(Tmp.first), const_cast<Array::Element *>(Tmp.second));
  }

  /// Returns delinearized representation of a specified element of an array.
  std::pair<const Array *, const Array::Element *>
    findElement(const llvm::Value *ElementPtr) const;

  /// Returns an array which starts at a specified address.
  Array * findArray(const llvm::Value *BasePtr) {
    return const_cast<Array *>(
      static_cast<const DelinearizeInfo *>(this)->findArray(BasePtr));
  }

  /// Returns an array which starts at a specified address.
  const Array * findArray(const llvm::Value *BasePtr) const;

  /// Returns list of all delinearized arrays.
  ArraySet & getArrays() noexcept { return mArrays; }

  /// Returns list of all delinearized arrays.
  const ArraySet & getArrays() const noexcept { return mArrays; }

  void fillElementsMap();

  /// Remove all available information.
  void clear() {
    mArrays.clear();
    mElements.clear();
  }

private:
  ArraySet mArrays;
  ElementMap mElements;
};
}

namespace llvm {
class ArraySubscriptDelinearizePass :
  public FunctionPass, private bcl::Uncopyable {
public:

  typedef std::map <Instruction *, SmallVector<
    std::pair<const SCEV *, const SCEV *>, 3>> ArraySubscriptDelinearizeInfo;

  typedef std::set<Instruction *> ArraySubscriptSet;

  static char ID;

  ArraySubscriptDelinearizePass() : llvm::FunctionPass(ID) {
    initializeArraySubscriptDelinearizePassPass(*llvm::PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void print(raw_ostream &OS, const Module *M) const override;

  void releaseMemory() override { mDelinearizeInfo.clear(); }

  const tsar::DelinearizeInfo & getDelinearizeInfo() const {
    return mDelinearizeInfo;
  }

private:
  tsar::DelinearizeInfo mDelinearizeInfo;
};
}


#endif //TSAR_ARRAY_SUBSCRIPT_DELINEARIZE_H