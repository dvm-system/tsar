//===--- tsar_df_alloca.h - Data Flow Framework ------ ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines abstractions to access information obtained from data-flow
// analysis and associated with allocas.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DF_ALLOCA_H
#define TSAR_DF_ALLOCA_H

#include <llvm/ADT/SmallPtrSet.h>

namespace llvm {
class AllocaInst;
}

namespace tsar {
/// \brief Representation of a data-flow value formed by a set of allocas.
/// 
/// A data-flow value is a set of allocas for which a number of operations
/// is defined.
class AllocaDFValue {
  typedef llvm::SmallPtrSet<llvm::AllocaInst *, 64> AllocaSet;
  // There are two kind of values. The KIND_FULL kind means that the set of
  // variables is full and contains all variables used in the analyzed program.
  // The KIND_MASK kind means that the set contains variables located in the 
  // alloca collection (mAllocas). This is internal information which is
  // neccessary to safely and effectively implement a number of operations
  // which is permissible for a arbitrary set of variables.
  enum Kind {
    FIRST_KIND,
    KIND_FULL = FIRST_KIND,
    KIND_MASK,
    LAST_KIND = KIND_MASK,
    INVALID_KIND,
    NUMBER_KIND = INVALID_KIND
  };
  AllocaDFValue(Kind K) : mKind(K) {
    assert(FIRST_KIND <= K && K <= LAST_KIND &&
      "The specified kind is invalid!");
  }
public:
  /// Creats a value, which contains all variables used in the analyzed
  /// program.
  static AllocaDFValue fullValue() {
    return AllocaDFValue(AllocaDFValue::KIND_FULL);
  }

  /// Creates an empty value.
  static AllocaDFValue emptyValue() {
    return AllocaDFValue(AllocaDFValue::KIND_MASK);
  }

  /// Default constructor creates an empty value.
  AllocaDFValue() : AllocaDFValue(AllocaDFValue::KIND_MASK) {}

  /// \brief Calculates the difference between a set of allocas and a set
  /// which is represented as a data-flow value.
  ///
  /// \param [in] AIBegin Iterator that points to the beginning of the allocas
  /// set.
  /// \param [in] AIEnd Iterator that points to the ending of the allocas set.
  /// \param [in] Value Data-flow value.
  /// \param [out] Result It contains the result of this operation.
  /// The following operation should be provided:
  /// - void ResultSet::insert(llvm::AllocaInst *).
  template<class alloca_iterator, class ResultSet>
  static void difference(const alloca_iterator &AIBegin,
    const alloca_iterator &AIEnd,
    const AllocaDFValue &Value, ResultSet &Result) {
    //If all allocas are contained in Value or range of iterators is empty,
    //than Result should be empty.
    if (Value.mKind == KIND_FULL || AIBegin == AIEnd)
      return;
    if (Value.mAllocas.empty())
      Result.insert(AIBegin, AIEnd);
    for (alloca_iterator I = AIBegin; I != AIEnd; ++I)
      if (Value.exist(*I))
        Result.insert(*I);
  }

  /// Destructor.
  ~AllocaDFValue() { mKind = INVALID_KIND; }

  /// Move constructor.
  AllocaDFValue(AllocaDFValue &&that) :
    mKind(that.mKind), mAllocas(std::move(that.mAllocas)) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
  }

  /// Copy constructor.
  AllocaDFValue(const AllocaDFValue &that) :
    mKind(that.mKind), mAllocas(that.mAllocas) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
  }

  /// Move assignment operator.
  AllocaDFValue & operator=(AllocaDFValue &&that) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
    if (this != &that) {
      mKind = that.mKind;
      mAllocas = std::move(that.mAllocas);
    }
    return *this;
  }

  /// Copy assignment operator.
  AllocaDFValue & operator=(const AllocaDFValue &that) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    assert(that.mKind != INVALID_KIND && "Collection is corrupted!");
    if (this != &that) {
      mKind = that.mKind;
      mAllocas = that.mAllocas;
    }
    return *this;
  }

  /// Returns true if the value contains the specified alloca.
  bool exist(llvm::AllocaInst *AI) const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_FULL || mAllocas.count(AI);
  }

  /// Returns true if the value does not contain any alloca.
  bool empty() const {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    return mKind == KIND_MASK && mAllocas.empty();
  }

  /// Removes all allocas from the value.
  void clear() {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    mKind = KIND_MASK;
    mAllocas.clear();
  }

  /// Inserts a new alloca into the value, returns false if it already exists.
  bool insert(llvm::AllocaInst *AI) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
    return mKind == KIND_FULL || mAllocas.insert(AI);
#else
    return mKind == KIND_FULL || mAllocas.insert(AI).second;
#endif
  }

  /// Inserts all allocas from the range into the value, returns false
  /// ifnothing has been added.
  template<class alloca_iterator >
  bool insert(const alloca_iterator &AIBegin, const alloca_iterator &AIEnd) {
    assert(mKind != INVALID_KIND && "Collection is corrupted!");
    if (mKind == KIND_FULL)
      return false;
    bool isChanged = false;
    for (alloca_iterator I = AIBegin; I != AIEnd; ++I)
#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 6)
      isChanged = mAllocas.insert(*I) || isChanged;
#else
      isChanged = mAllocas.insert(*I).second || isChanged;
#endif
    return isChanged;
  }

  /// Realizes intersection between two values.
  bool intersect(const AllocaDFValue &with);

  /// Realizes merger between two values.
  bool merge(const AllocaDFValue &with);

  /// Compares two values.
  bool operator==(const AllocaDFValue &RHS) const;

  /// Compares two values.
  bool operator!=(const AllocaDFValue &RHS) const { return !(*this == RHS); }
private:
  Kind mKind;
  AllocaSet mAllocas;
};

/// \brief This calculates the difference between a set of allocas and a set
/// which is represented as a data-flow value.
///
/// \param [in] AIBegin Iterator that points to the beginning of the allocas
/// set.
/// \param [in] AIEnd Iterator that points to the ending of the allocas set.
/// \param [in] Value Data-flow value.
/// \param [out] Result It contains the result of this operation.
/// The following operation should be provided:
/// - void ResultSet::insert(llvm::AllocaInst *).
template<class alloca_iterator, class ResultSet>
static void difference(const alloca_iterator &AIBegin,
  const alloca_iterator &AIEnd,
  const AllocaDFValue &Value, ResultSet &Result) {
  AllocaDFValue::difference(AIBegin, AIEnd, Value, Result);
}
}

#endif//TSAR_DF_ALLOCA_H
