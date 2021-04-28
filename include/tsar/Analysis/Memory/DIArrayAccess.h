//===- DIArrayAccess.h --- Array Access Collection (Metadata) ---*- C++ -*-===//
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
// This file implements a pass to collect array accesses in a program.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DI_ARRAY_ACCESS_H
#define TSAR_DI_ARRAY_ACCESS_H

#include "tsar/ADT/DenseMapTraits.h"
#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Memory/Passes.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include "tsar/Support/Tags.h"
#include <bcl/Equation.h>
#include <llvm/ADT/APSInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/ilist_node.h>
#include <llvm/InitializePasses.h>
#include <vector>

namespace tsar {
class DIArraySubscript;
class DIArrayAccessInfo;
class DIMemory;

/// Single array access.
class DIArrayAccess
    : public llvm::ilist_node<DIArrayAccess, llvm::ilist_tag<Pool>>,
      public llvm::ilist_node<DIArrayAccess, llvm::ilist_tag<Sibling>> {
  using SubscriptList = llvm::SmallVector<std::unique_ptr<DIArraySubscript>, 5>;

  using DerefFnTy = DIArraySubscript *(*)(SubscriptList::value_type &);
  using ConstDerefFnTy =
      const DIArraySubscript *(*)(const SubscriptList::value_type &);
  static DIArraySubscript *subscript_helper(SubscriptList::value_type &V) {
    return V.get();
  }
  static const DIArraySubscript *
  subscript_helper(const SubscriptList::value_type &V) {
    return V.get();
  }

public:
  using Scope = ObjectID;
  using Array = DIMemory *;

  using iterator = llvm::mapped_iterator<SubscriptList::iterator, DerefFnTy>;
  using const_iterator =
      llvm::mapped_iterator<SubscriptList::const_iterator, ConstDerefFnTy>;

  DIArrayAccess(Array Array, Scope Parent, unsigned NumberOfSubscripts,
                AccessInfo R, AccessInfo W)
      : mArray(Array), mParent(Parent), mReadInfo(R), mWriteInfo(W) {
    mSubscripts.reserve(NumberOfSubscripts);
    for (unsigned I = 0; I < NumberOfSubscripts; ++I)
      mSubscripts.emplace_back();
  }

  /// Return an array to be accessed.
  Array getArray() const noexcept { return mArray; }

  /// Return innermost analysis scope which owns this access.
  Scope getParent() const noexcept { return mParent; }

  bool isReadOnly() const noexcept { return mWriteInfo == AccessInfo::No; }
  bool isWriteOnly() const noexcept { return mReadInfo == AccessInfo::No; }

  AccessInfo getWriteInfo() const noexcept { return mWriteInfo; }
  AccessInfo getReadInfo() const noexcept { return mReadInfo; }

  /// Return number of subscripts.
  unsigned size() const { return mSubscripts.size(); }

  /// Return true if there are no subscript expressions.
  bool empty() const { return mSubscripts.empty(); }

  iterator begin() { return {mSubscripts.begin(), subscript_helper}; }
  iterator end() { return {mSubscripts.end(), subscript_helper}; }

  const_iterator begin() const {
    return {mSubscripts.begin(), subscript_helper};
  }
  const_iterator end() const { return {mSubscripts.end(), subscript_helper}; }

  DIArraySubscript *operator[](unsigned DimIdx) {
    return mSubscripts[DimIdx].get();
  }

  const DIArraySubscript *operator[](unsigned DimIdx) const {
    return mSubscripts[DimIdx].get();
  }

  /// Create subscript of a specified type `T`.
  template <typename T, typename... Args>
  std::enable_if_t<std::is_base_of<DIArraySubscript, T>::value, T *>
  make(Args &&... Arg);

  /// Reset a specified subscript.
  void reset(unsigned DimIdx) { mSubscripts[DimIdx].reset(); }

private:
  Array mArray;
  Scope mParent;
  AccessInfo mWriteInfo;
  AccessInfo mReadInfo;
  SubscriptList mSubscripts;
};

class DIAffineSubscript;

class DIArraySubscript {
  template <typename Function> struct Apply {
    template <typename T> void operator()() {
      if (llvm::isa<T>(S))
        F(llvm::cast<T>(S));
    }
    DIArraySubscript &S;
    Function &F;
  };

  using KindList = bcl::TypeList<DIAffineSubscript>;

public:
  using Kind = uint8_t;

  /// Return kind for a specified type of subscript.
  template <typename T> static Kind kindof() { return KindList::index_of<T>(); }

  using Scope = DIArrayAccess::Scope;
  using Array = DIArrayAccess::Array;

  virtual ~DIArraySubscript() {}

  Kind getKind() const noexcept { return mKind; }

  DIArrayAccess *getAccess() noexcept { return mAccess; }
  const DIArrayAccess *getAccess() const noexcept { return mAccess; }

  unsigned getDimension() const noexcept { return mDimension; }
  Array getArray() const noexcept { return mAccess->getArray(); }

  /// Conver this subscript to an appropriate derived type and apply a specified
  /// function to it.
  template <typename Function> void apply(Function &&F) {
    KindList::for_each_type(Apply<Function>{*this, F});
  }

  /// Print subscript.
  void print(llvm::raw_ostream &OS) const {
    const_cast<DIArraySubscript *>(this)->apply(
        [&OS](const auto &To) { To.print(OS); });
  }

protected:
  DIArraySubscript(DIArrayAccess *Access, unsigned Dimension, Kind K)
      : mAccess(Access), mDimension(Dimension), mKind(K) {
    assert(Access && "Array access must not be null!");
    assert(Dimension < Access->size() && "Dimension index is out of range!");
  }

private:
  DIArrayAccess *mAccess;
  Kind mKind;
  unsigned mDimension;
};

template <typename T, typename... Args>
std::enable_if_t<std::is_base_of<DIArraySubscript, T>::value, T *>
DIArrayAccess::make(Args &&... Arg) {
  auto DimensionAccess = std::make_unique<T>(this, std::forward<Args>(Arg)...);
  return llvm::cast<T>((mSubscripts[DimensionAccess->getDimension()] =
                            std::move(DimensionAccess))
                           .get());
}

class DIAffineSubscript : public DIArraySubscript {
public:
  struct Symbol {
    enum SymbolKind {
      SK_Constant,
      // The value in term is the sum of a constant and a variable.
      SK_Variable,
      // The value in term is the sum of a constant and a start value of an
      // induction.
      SK_Induction,
    };

    Symbol(const llvm::APSInt &C) :
        Kind(SK_Constant), Constant(C), Variable(nullptr) {}
    Symbol(SymbolKind SK, const llvm::APSInt &C, const WeakDIMemoryHandle DIM) :
        Kind(SK), Constant(C), Variable(DIM) { }

    SymbolKind Kind;
    llvm::APSInt Constant;
    WeakDIMemoryHandle Variable;
  };

  using Monom = milp::AMonom<Scope, Symbol>;

  static bool classof(const DIArraySubscript *A) {
    return A->getKind() == DIArraySubscript::kindof<DIAffineSubscript>();
  }

  DIAffineSubscript(DIArrayAccess *Access, unsigned Dimension, const Symbol &S)
      : DIArraySubscript(Access, Dimension,
                         DIArraySubscript::kindof<DIAffineSubscript>()),
        mSymbol(S) {}

  DIAffineSubscript(DIArrayAccess *Access, unsigned Dimension,
                    const DIAffineSubscript &Subscript)
      : DIArraySubscript(Access, Dimension,
                         DIArraySubscript::kindof<DIAffineSubscript>()),
        mMonoms(Subscript.mMonoms), mSymbol(Subscript.mSymbol) {}

  DIAffineSubscript(DIArrayAccess *Access, unsigned Dimension,
                    DIAffineSubscript &&Subscript)
      : DIArraySubscript(Access, Dimension,
                         DIArraySubscript::kindof<DIAffineSubscript>()),
        mMonoms(std::move(Subscript.mMonoms)),
        mSymbol(std::move(Subscript.mSymbol)) {}

  void addMonom(const Monom &M) { mMonoms.push_back(M); }
  void addMonom(Monom &&M) { mMonoms.push_back(std::move(M)); }

  void emplaceMonom(ObjectID Loop, const Symbol &Factor) {
    mMonoms.emplace_back(Loop, Factor);
  }

  unsigned getNumberOfMonoms() const { return mMonoms.size(); }
  const Monom & getMonom(unsigned Idx) const { return mMonoms[Idx]; }

  const Symbol & getSymbol() const noexcept { return mSymbol; }

  /// Print subscript.
  ///
  /// Loop identifier, which is unique across a single file, is used instead of
  /// the name of a corresponding induction variable.
  void print(llvm::raw_ostream &OS) const;

private:
  llvm::SmallVector<Monom, 2> mMonoms;
  Symbol mSymbol;
};

/// This track accesses to array accross RAUW.
class DIArrayHandle final : public CallbackDIMemoryHandle {
public:
  DIArrayHandle(DIMemory *M, DIArrayAccessInfo *AccessInfo = nullptr)
      : CallbackDIMemoryHandle(M), mAccessInfo(AccessInfo) {}

  DIArrayHandle &operator=(DIMemory *M) {
    return *this = DIArrayHandle(M, mAccessInfo);
  }

  operator DIMemory *() const {
    return CallbackDIMemoryHandle::operator tsar::DIMemory *();
  }

private:
  void deleted() override;
  void allUsesReplacedWith(DIMemory *M) override;

  DIArrayAccessInfo *mAccessInfo;
};

class DIArrayAccessInfo {
  /// Sorted list of accesses.
  ///
  /// Accesses are sorted according to scopes which contain them.
  /// List takes ownership of accesses it contains.
  ///
  /// Example:
  /// A[f()] = A[10];
  /// for(... I ...) {
  ///    A[I] = B[g()]; A[I + 1] = B[I];
  ///    for (... J ...)
  ///      C[I] = ...
  /// }
  /// List of accesses may be the following:
  /// A[f()], A[10], A[I], B[g()] A[I + 1], B[I], C[I]
  using AccessPool = llvm::ilist<DIArrayAccess, llvm::ilist_tag<Pool>>;

  /// List of accesses which subsequently connect sorted lists of accesses to
  /// the same array.
  ///
  /// Accesses to the same array are sorted according to scopes which contain
  /// them.
  ///
  /// For the previous example this list may be the following:
  /// A[f()], A[10], A[I], A[I + 1], B[g()], B[I], C[I]
  using ArrayList = llvm::simple_ilist<DIArrayAccess, llvm::ilist_tag<Sibling>>;

public:
  using Scope = DIArrayAccess::Scope;
  using Array = DIArrayAccess::Array;

private:
  struct Begin {};
  struct End {};
  struct Parent {};

  /// Map from array to its accesses in a scope.
  ///
  /// This map is constructed in the following way
  /// { Array -> {FirstAccessInScope, LastAccessInScope + 1}, ... }
  using ArrayToAccessMap = llvm::DenseMap<
      Array, std::tuple<ArrayList::iterator, ArrayList::iterator>,
      llvm::DenseMapInfo<Array>,
      TaggedDenseMapTuple<bcl::tagged<Array, Array>,
                          bcl::tagged<ArrayList::iterator, Begin>,
                          bcl::tagged<ArrayList::iterator, End>>>;

  /// Map from scope to array accesses.
  using ScopeToAccessMap = llvm::DenseMap<
      Scope,
      std::tuple<Scope, AccessPool::iterator, AccessPool::iterator,
                 ArrayToAccessMap>,
      llvm::DenseMapInfo<Scope>,
      TaggedDenseMapTuple<bcl::tagged<Scope, Scope>, bcl::tagged<Scope, Parent>,
                          bcl::tagged<AccessPool::iterator, Begin>,
                          bcl::tagged<AccessPool::iterator, End>,
                          bcl::tagged<ArrayToAccessMap, ArrayToAccessMap>>>;

public:
  using iterator = AccessPool::iterator;
  using const_iterator = AccessPool::const_iterator;

  using array_iterator = ArrayList::iterator;
  using const_array_iterator = ArrayList::const_iterator;

  iterator begin() { return mAccesses.begin(); }
  iterator end() { return mAccesses.end(); }
  llvm::iterator_range<iterator> accesses() {
    return llvm::make_range(begin(), end());
  }

  const_iterator begin() const { return mAccesses.begin(); }
  const_iterator end() const { return mAccesses.end(); }
  llvm::iterator_range<const_iterator> accesses() const {
    return llvm::make_range(begin(), end());
  }

  using scope_iterator = iterator;
  using const_scope_iterator = const_iterator;

  scope_iterator scope_begin(const Scope &S) {
    auto ScopeItr = mScopeToAccesses.find(S);
    return ScopeItr == mScopeToAccesses.end()
               ? mAccesses.end()
               : scope_iterator(ScopeItr->get<Begin>());
  }
  scope_iterator scope_end(const Scope &S) {
    auto ScopeItr = mScopeToAccesses.find(S);
    return ScopeItr == mScopeToAccesses.end()
               ? mAccesses.end()
               : scope_iterator(ScopeItr->get<End>());
  }
  llvm::iterator_range<scope_iterator> scope_accesses(const Scope &S) {
    auto ScopeItr = mScopeToAccesses.find(S);
    return ScopeItr == mScopeToAccesses.end()
               ? llvm::make_range(mAccesses.end(), mAccesses.end())
               : llvm::make_range(scope_iterator(ScopeItr->get<Begin>()),
                                  scope_iterator(ScopeItr->get<End>()));
  }

  const_scope_iterator scope_begin(const Scope &S) const {
    auto ScopeItr = mScopeToAccesses.find(S);
    return ScopeItr == mScopeToAccesses.end()
               ? mAccesses.end()
               : const_scope_iterator(ScopeItr->get<Begin>());
  }
  const_scope_iterator scope_end(const Scope &S) const {
    auto ScopeItr = mScopeToAccesses.find(S);
    return ScopeItr == mScopeToAccesses.end()
               ? mAccesses.end()
               : const_scope_iterator(ScopeItr->get<End>());
  }

  /// Iterate over all accesses in a scope.
  llvm::iterator_range<const_scope_iterator>
  scope_accesses(const Scope &S) const {
    auto ScopeItr = mScopeToAccesses.find(S);
    return ScopeItr == mScopeToAccesses.end()
               ? llvm::make_range(mAccesses.end(), mAccesses.end())
               : llvm::make_range(const_scope_iterator(ScopeItr->get<Begin>()),
                                  const_scope_iterator(ScopeItr->get<End>()));
  }

  array_iterator array_begin(const Array &A) {
    auto ArrayItr = mArrayToAccesses.find(A);
    return ArrayItr == mArrayToAccesses.end() ? mArrayAccesses.end()
                                              : ArrayItr->get<Begin>();
  }
  array_iterator array_end(const Array &A) {
    auto ArrayItr = mArrayToAccesses.find(A);
    return ArrayItr == mArrayToAccesses.end() ? mArrayAccesses.end()
                                              : ArrayItr->get<End>();
  }

  /// Iterate over all accesses to a specified array.
  llvm::iterator_range<array_iterator> array_accesses(const Array &A) {
    auto ArrayItr = mArrayToAccesses.find(A);
    return ArrayItr == mArrayToAccesses.end()
               ? llvm::make_range(mArrayAccesses.end(), mArrayAccesses.end())
               : llvm::make_range(ArrayItr->get<Begin>(), ArrayItr->get<End>());
  }

  const_array_iterator array_begin(const Array &A) const {
    auto ArrayItr = mArrayToAccesses.find(A);
    return ArrayItr == mArrayToAccesses.end() ? mArrayAccesses.end()
                                              : ArrayItr->get<Begin>();
  }
  const_array_iterator array_end(const Array &A) const {
    auto ArrayItr = mArrayToAccesses.find(A);
    return ArrayItr == mArrayToAccesses.end() ? mArrayAccesses.end()
                                              : ArrayItr->get<End>();
  }

  /// Iterate over all accesses to a specified array.
  llvm::iterator_range<const_array_iterator>
  array_accesses(const Array &A) const {
    auto ArrayItr = mArrayToAccesses.find(A);
    return ArrayItr == mArrayToAccesses.end()
               ? llvm::make_range(mArrayAccesses.end(), mArrayAccesses.end())
               : llvm::make_range(const_array_iterator(ArrayItr->get<Begin>()),
                                  const_array_iterator(ArrayItr->get<End>()));
  }

  array_iterator array_begin(const Array &A, const Scope &S) {
    auto ScopeItr = mScopeToAccesses.find(S);
    if (ScopeItr == mScopeToAccesses.end())
      return mArrayAccesses.end();
    auto ArrayItr = ScopeItr->get<ArrayToAccessMap>().find(A);
    return ArrayItr == ScopeItr->get<ArrayToAccessMap>().end()
               ? mArrayAccesses.end()
               : ArrayItr->get<Begin>();
  }
  array_iterator array_end(const Array &A, const Scope &S) {
    auto ScopeItr = mScopeToAccesses.find(S);
    if (ScopeItr == mScopeToAccesses.end())
      return mArrayAccesses.end();
    auto ArrayItr = ScopeItr->get<ArrayToAccessMap>().find(A);
    return ArrayItr == ScopeItr->get<ArrayToAccessMap>().end()
               ? mArrayAccesses.end()
               : ArrayItr->get<End>();
  }

  /// Iterate over all accesses to a specified array in a scope.
  llvm::iterator_range<array_iterator> array_accesses(const Array &A,
                                                      const Scope &S) {
    auto ScopeItr = mScopeToAccesses.find(S);
    if (ScopeItr == mScopeToAccesses.end())
      return llvm::make_range(mArrayAccesses.end(), mArrayAccesses.end());
    auto ArrayItr = ScopeItr->get<ArrayToAccessMap>().find(A);
    return ArrayItr == ScopeItr->get<ArrayToAccessMap>().end()
               ? llvm::make_range(mArrayAccesses.end(), mArrayAccesses.end())
               : llvm::make_range(ArrayItr->get<Begin>(), ArrayItr->get<End>());
  }

  const_array_iterator array_begin(const Array &A, const Scope &S) const {
    auto ScopeItr = mScopeToAccesses.find(S);
    if (ScopeItr == mScopeToAccesses.end())
      return mArrayAccesses.end();
    auto ArrayItr = ScopeItr->get<ArrayToAccessMap>().find(A);
    return ArrayItr == ScopeItr->get<ArrayToAccessMap>().end()
               ? mArrayAccesses.end()
               : ArrayItr->get<Begin>();
  }
  const_array_iterator array_end(const Array &A, const Scope &S) const {
    auto ScopeItr = mScopeToAccesses.find(S);
    if (ScopeItr == mScopeToAccesses.end())
      return mArrayAccesses.end();
    auto ArrayItr = ScopeItr->get<ArrayToAccessMap>().find(A);
    return ArrayItr == ScopeItr->get<ArrayToAccessMap>().end()
               ? mArrayAccesses.end()
               : ArrayItr->get<End>();
  }

  /// Iterate over all accesses to a specified array in a scope.
  llvm::iterator_range<const_array_iterator>
  array_accesses(const Array &A, const Scope &S) const {
    auto ScopeItr = mScopeToAccesses.find(S);
    if (ScopeItr == mScopeToAccesses.end())
      return llvm::make_range(mArrayAccesses.end(), mArrayAccesses.end());
    auto ArrayItr = ScopeItr->get<ArrayToAccessMap>().find(A);
    return ArrayItr == ScopeItr->get<ArrayToAccessMap>().end()
               ? llvm::make_range(mArrayAccesses.end(), mArrayAccesses.end())
               : llvm::make_range(const_array_iterator(ArrayItr->get<Begin>()),
                                  const_array_iterator(ArrayItr->get<End>()));
  }

  /// Return list of scopes (from the innermost to the outermost) which contain
  /// specified access.
  void scopes(const DIArrayAccess &Access,
              llvm::SmallVectorImpl<Scope> &Scopes) {
    auto CurrScope = Access.getParent();
    for (;;) {
      Scopes.push_back(CurrScope);
      auto ScopeItr = mScopeToAccesses.find(CurrScope);
      assert(ScopeItr != mScopeToAccesses.end() &&
             "Unknown scope! Does the access located in the list of accesses?");
      if (!ScopeItr->get<Parent>())
        return;
      CurrScope = ScopeItr->get<Parent>();
    }
  }

  std::size_t size() const { return mAccesses.size(); }

  /// Add access to the list of accesses. This class takes ownership of
  /// a specified access.
  ///
  /// \param [in] Access A new access to insert.
  /// \param [in] Scopes Stack of scopes which contain a specified access.
  /// The last one scope must be an outerrmost scope which explicitly contains
  /// a specified access.
  void add(DIArrayAccess *Access, llvm::ArrayRef<Scope> Scopes);

  /// Remove all accesses to a specified array from the list.
  void erase(const Array &V);

  void clear() {
    mArrayToAccesses.clear();
    mScopeToAccesses.clear();
    mArrayAccesses.clear();
    mAccesses.clear();
  }

  void print(llvm::raw_ostream &OS) const;
  void dump() const;

private:
  /// Traverse scopes in upward order and look up for the first scope which
  /// access a specified array.
  ///
  /// \return Description of the accesses to the specified array in the found
  /// scope.
  /// \post `VisitedScopes` contains all traversed scopes.
  ArrayToAccessMap::iterator findOuterScopeWithArrayAccess(
      const Array &A, const ScopeToAccessMap::iterator &ScopeItr,
      llvm::SmallVectorImpl<ScopeToAccessMap::iterator> &VisitedScopes) {
    VisitedScopes.push_back(ScopeItr);
    auto ArrayItr = ScopeItr->get<ArrayToAccessMap>().find(A);
    if (ArrayItr == ScopeItr->get<ArrayToAccessMap>().end())
      if (ScopeItr->get<Parent>())
        return findOuterScopeWithArrayAccess(
            A, mScopeToAccesses.find(ScopeItr->get<Parent>()), VisitedScopes);
      else
        return ArrayItr;
    return ArrayItr;
  }

  void printScope(
      const ScopeToAccessMap::const_iterator &ScopeItr, unsigned Offset,
      unsigned OffsetStep, llvm::Optional<unsigned> DWLang,
      llvm::DenseMap<const DIArrayAccess *, AccessPool::size_type> &AccessMap,
      llvm::DenseMap<Scope, std::vector<ScopeToAccessMap::const_iterator>>
          &Children,
      llvm::raw_ostream &OS) const;

  llvm::DenseSet<DIArrayHandle> mArrays;
  AccessPool mAccesses;
  ArrayList mArrayAccesses;
  ScopeToAccessMap mScopeToAccesses;
  ArrayToAccessMap mArrayToAccesses;
};
} // namespace tsar

namespace llvm {
template <>
struct DenseMapInfo<tsar::DIArrayHandle>
    : public DenseMapInfo<tsar::DIMemory *> {};

class DIArrayAccessWrapper : public ModulePass, bcl::Uncopyable {
public:
  static char ID;
  DIArrayAccessWrapper() : ModulePass(ID) {
    initializeDIArrayAccessWrapperPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void releaseMemory() override {
    if (mIsInfoOwner && mAccessInfo)
      delete mAccessInfo;
    mAccessInfo = nullptr;
  }

  tsar::DIArrayAccessInfo *getAccessInfo() noexcept { return mAccessInfo; }
  const tsar::DIArrayAccessInfo *getAccessInfo() const noexcept {
    return mAccessInfo;
  }

  void print(llvm::raw_ostream &OS, const llvm::Module *) const override {
    if (mAccessInfo)
      mAccessInfo->print(OS);
  }

private:
  bool mIsInfoOwner = false;
  tsar::DIArrayAccessInfo *mAccessInfo = nullptr;
};
} // namespace llvm
#endif // TSAR_DI_ARRAY_ACCESS_H
