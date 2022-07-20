//===- Parallelization.h - Parallelization Capabilities ----------*- C++ -*===//
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
// This file proposes data structures to described the parallelization
// capabilities of an analyzed program.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PARALLELIZATION_H
#define TSAR_PARALLELIZATION_H

#include "tsar/ADT/DenseMapTraits.h"
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/iterator_range.h>
#include <llvm/ADT/PointerUnion.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/TinyPtrVector.h>
#include <llvm/Support/Casting.h>
#include <vector>

namespace llvm {
class BasicBlock;
class Function;
class Instruction;
class MDNode;
}

namespace tsar {
LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// This represents a parallel construct.
class ParallelItem {
  enum Flags : uint8_t {
    NoProperty = 0,
    Final = 1u << 0,
    ChildPossible = 1u << 1,
    Marker = 1u << 2,
    LLVM_MARK_AS_BITMASK_ENUM(Marker)
  };

public:
  using parent_iterator = llvm::TinyPtrVector<ParallelItem *>::iterator;
  using const_parent_iterator =
      llvm::TinyPtrVector<ParallelItem *>::const_iterator;

  explicit ParallelItem(unsigned Kind, bool IsFinal)
      : ParallelItem(Kind, IsFinal, false, false) {}

  virtual ~ParallelItem();

  /// Return true if this parallel item could be merged with item constructed
  /// from a specified values.
  template <typename... Ts> bool isMergeableWith(Ts &&...V) const noexcept {
    return false;
  }

  /// Return user-defined kind of a parallel item.
  unsigned getKind() const noexcept { return mKind; }

  /// Return true if this item may contain nested parallel items.
  bool isChildPossible() const noexcept { return mFlags & ChildPossible; }

  /// Return true if this item has been finalized. For hierarchical source code
  /// constructs (for example loops) it means that nested constructs should not
  /// be analyzed.
  ///
  /// Use finalize() method to mark this item as final.
  bool isFinal() const noexcept { return mFlags & Final; }

  /// Return true if this is a marker which is auxiliary construction.
  bool isMarker() const noexcept { return mFlags & Marker; }

  parent_iterator parent_begin() { return mParents.begin(); }
  parent_iterator parent_end() { return mParents.end(); }

  const_parent_iterator parent_begin() const { return mParents.begin(); }
  const_parent_iterator parent_end() const { return mParents.end(); }

  llvm::iterator_range<parent_iterator> parents() {
    return llvm::make_range(parent_begin(), parent_end());
  }

  llvm::iterator_range<const_parent_iterator> parents() const {
    return llvm::make_range(parent_begin(), parent_end());
  }

  ParallelItem * parent_front() { return mParents.front(); }
  const ParallelItem * parent_front() const { return mParents.front(); }

  void parent_insert(ParallelItem *PI) {
    mParents.push_back(PI);
  }

  parent_iterator parent_erase(parent_iterator I) { return mParents.erase(I); }
  void parent_clear() { mParents.clear(); }

  std::size_t parent_size() const { return mParents.size(); }
  bool parent_empty() const { return mParents.empty(); }

  /// Mark item as final.
  ///
  /// \attention Overridden methods have to call this one to set corresponding
  /// flags.
  virtual void finalize() { mFlags |= Final; }

protected:
  ParallelItem(unsigned Kind, bool IsFinal, bool IsMarker, bool IsChildPossible)
      : mFlags(NoProperty), mKind(Kind) {
    if (IsFinal)
      mFlags |= Final;
    if (IsMarker)
      mFlags |= Marker;
    if (IsChildPossible)
      mFlags |= ChildPossible;
  }

private:
  llvm::TinyPtrVector<ParallelItem *> mParents;
  Flags mFlags;
  unsigned mKind;
};

/// This represents a parallel construct which may contain other constructs
/// (for example DVMH region contains parallel loops).
class ParallelLevel : public ParallelItem {
public:
  using child_iterator = std::vector<ParallelItem *>::iterator;
  using const_child_iterator = std::vector<ParallelItem *>::const_iterator;

  static bool classof(const ParallelItem *Item) noexcept {
    return Item->isChildPossible();
  }

  explicit ParallelLevel(unsigned Kind, bool IsFinal)
      : ParallelItem(Kind, IsFinal, false, true) {}

  ~ParallelLevel() {
    for (auto *Child : mChildren)
      if (auto I{llvm::find(Child->parents(), this)}; I != Child->parent_end())
        Child->parent_erase(I);
  }

  child_iterator child_insert(ParallelItem *Item) {
    assert(!Item->isMarker());
    mChildren.push_back(Item);
    return std::prev(mChildren.end());
  }

  child_iterator child_erase(child_iterator I) { return mChildren.erase(I); }

  child_iterator child_begin() { return mChildren.begin(); }
  child_iterator child_end() { return mChildren.end(); }

  const_child_iterator child_begin() const { return mChildren.begin(); }
  const_child_iterator child_end() const { return mChildren.end(); }

  llvm::iterator_range<child_iterator> children() {
    return llvm::make_range(child_begin(), child_end());
  }

  llvm::iterator_range<const_child_iterator> children() const {
    return llvm::make_range(child_begin(), child_end());
  }

  std::size_t child_size() const noexcept { return mChildren.size(); }
  bool child_empty() const noexcept { return mChildren.empty(); }

private:
  std::vector<ParallelItem *> mChildren;
};

/// This is auxiliary item which references to an item of a specified type.
///
/// Use get/setParent() to access original item. This class is useful to mark
/// the end of a parallel construct in source code.
template<class ItemT>
class ParallelMarker : public ParallelItem {
public:
  static bool classof(const ParallelItem *Item) noexcept {
    return Item->isMarker() && !Item->parent_empty() &&
           llvm::find_if(Item->parents(), [](const auto *Parent) {
             return llvm::isa<ItemT>(Parent);
           }) != Item->parent_end();
  }

  ParallelMarker(unsigned Kind, ItemT *For)
      : ParallelItem(Kind, true, true, false) {
    assert(For && "Marker must be attached to an item!");
    parent_insert(For);
  }

  /// Return a parallel item related to this marker.
  ItemT *getBase() {
    auto I{llvm::find_if(parents(), [](const auto *Parent) {
      return llvm::isa<ItemT>(Parent);
    })};
    assert(I != parent_end() &&
           "A base directive for the marker must be known!");
    return llvm::cast<ItemT>(*I);
  }

  /// Return a parallel item related to this marker.
  const ItemT *getBase() const {
    auto I{llvm::find_if(parents(), [](const auto *Parent) {
      return llvm::isa<ItemT>(Parent);
    })};
    assert(I != parent_end() &&
           "A base directive for the marker must be known!");
    return llvm::cast<ItemT>(*I);
  }
};

inline ParallelItem::~ParallelItem() {
  if (!isMarker())
    for (auto *Parent : parents())
      if (auto *P{llvm::dyn_cast<ParallelLevel>(Parent)})
        if (auto ChildItr{llvm::find(P->children(), this)};
            ChildItr != P->child_end())
          P->child_erase(ChildItr);
}

/// Sequence which determines an order of parallel constructs in a source code.
/// This is similar to a basic block in a control-flow graph.
using ParallelBlock = llvm::SmallVector<std::unique_ptr<ParallelItem>, 4>;

/// This determine location in a source code to insert parallel constructs.
struct ParallelLocation {
  /// Source-code item which implies parallel constructs.
  llvm::PointerUnion<llvm::MDNode *, llvm::Value *> Anchor;

  /// Parallel constructs before a specified anchor.
  ParallelBlock Entry;

  /// Parallel constructs after a specified anchor.
  ParallelBlock Exit;
};

class ParallelItemRef;

/// This represents results of program parallelization.
class Parallelization {
  using ParallelLocationList = llvm::SmallVector<ParallelLocation, 1>;

  /// Collection of basic blocks with attached parallel blocks to them.
  using ParallelBlocks = llvm::DenseMap<
      llvm::BasicBlock *, ParallelLocationList,
      llvm::DenseMapInfo<llvm::BasicBlock *>,
      TaggedDenseMapPair<bcl::tagged<llvm::BasicBlock *, llvm::BasicBlock>,
                         bcl::tagged<ParallelLocationList, ParallelLocation>>>;

  /// Functions which contains parallel constructs.
  using ParallelFunctions = llvm::SmallPtrSet<llvm::Function *, 32>;

public:
  using iterator = ParallelBlocks::iterator;
  using const_iterator = ParallelBlocks::const_iterator;

  using location_iterator = ParallelLocationList::iterator;
  using location_const_iterator = ParallelLocationList::const_iterator;

  iterator begin() { return mParallelBlocks.begin(); }
  iterator end() { return mParallelBlocks.end(); }

  const_iterator begin() const { return mParallelBlocks.begin(); }
  const_iterator end() const { return mParallelBlocks.end(); }

  /// Return false if program has not been parallelized.
  bool empty() const { return mParallelBlocks.empty(); }

  /// Attach a new parallel block to a specified one and mark corresponding
  /// function as parallel.
  template<class... Ts>
  std::pair<iterator, bool> try_emplace(llvm::BasicBlock *BB, Ts &&... Args) {
    mParallelFuncs.insert(BB->getParent());
    return mParallelBlocks.try_emplace(BB, std::forward<Ts>(Args)...);
  }

  iterator find(const llvm::BasicBlock *BB) { return mParallelBlocks.find(BB); }
  const_iterator find(const llvm::BasicBlock *BB) const {
    return mParallelBlocks.find(BB);
  }

  void erase(llvm::BasicBlock *BB) { mParallelBlocks.erase(BB); }

  using function_iterator = ParallelFunctions::iterator;
  using const_function_iterator = ParallelFunctions::const_iterator;

  function_iterator func_begin() { return mParallelFuncs.begin(); }
  function_iterator func_end() { return mParallelFuncs.end(); }

  const_function_iterator func_begin() const { return mParallelFuncs.begin(); }
  const_function_iterator func_end() const { return mParallelFuncs.end(); }

  /// Add a new parallel item of a specified type 'ItemT' if it does not exist.
  template<typename ItemT, typename AnchorT, typename... Ts>
  std::pair<ParallelItemRef, bool> try_emplace(llvm::BasicBlock *BB,
    AnchorT Anchor, bool OnEntry = true, Ts &&... Args);

  /// Add a new parallel item of a specified type 'ItemT'.
  template <typename ItemT, typename AnchorT, typename... Ts>
  ParallelItemRef emplace(llvm::BasicBlock *BB, AnchorT Anchor,
                          bool OnEntry = true, Ts &&...Args);

  /// Look for a parallel item of a specified type 'ItemT'.
  template <typename ItemT, typename AnchorT>
  ParallelItemRef find(llvm::BasicBlock *BB, AnchorT Anchor,
                              bool OnEntry = true);

private:
  llvm::SmallPtrSet<llvm::Function *, 32> mParallelFuncs;
  ParallelBlocks mParallelBlocks;
};

/// This is a reference to a parallel item inside a parallelization.
class ParallelItemRef {
  enum State {
    Default = 0,
    Empty = 1u << 0,
    Tombstone = 1u << 1,
    OnExit = 1u << 2,
    LLVM_MARK_AS_BITMASK_ENUM(OnExit)
  };

public:
  ParallelItemRef(Parallelization::iterator PLocListItr,
                  Parallelization::location_iterator PLocItr,
                  ParallelBlock::iterator PIItr, bool IsOnEntry)
      : mPLocListItr(PLocListItr), mPLocItr(PLocItr), mPIItr(PIItr),
        mState(IsOnEntry ? Default : OnExit) {}

  ParallelItemRef() = default;

  /// Return parallel item or nullptr if reference is invalid.
  template<typename ItemT>
  ItemT * dyn_cast() const { return isValid() ? get<ItemT>() : nullptr; }

  /// Return parallel item.
  template<typename ItemT>
  ItemT *get() const {
    assert(isValid() && "Reference is invalid!");
    return llvm::cast<ItemT>(mPIItr->get());
  }

  /// Return edge from basic block to a list of parallel locations.
  Parallelization::iterator getPE() const {
    assert(isValid() && "Reference is invalid!");
    return mPLocListItr;
  }

  /// Return parallel location which contains a parallel item.
  Parallelization::location_iterator getPL() const {
    assert(isValid() && "Reference is invalid!");
    return mPLocItr;
  }

  /// Return parallel item
  ParallelBlock::iterator getPI() const {
    assert(isValid() && "Reference is invalid!");
    return mPIItr;
  }

  bool isOnExit() const noexcept {
    assert(isValid() && "Reference is invalid!");
    return mState & OnExit;
  }

  bool isOnEntry() const noexcept { return !isOnExit(); }

  bool isValid() const noexcept { return !isInvalid(); }
  bool isInvalid() const noexcept { return mState & (Empty | Tombstone); }
  operator bool() const noexcept { return isValid(); }

  ParallelItem *getUnchecked() { return mPIItr->get(); }
  const ParallelItem *getUnchecked() const { return mPIItr->get(); }

  ParallelItem & operator *() { return *mPIItr->get(); }
  ParallelItem * operator-> () { return mPIItr->get(); }

  operator ParallelItem *() {
    return isValid() ? mPIItr->get() : nullptr;
  }
  operator const ParallelItem *() const {
    return isValid() ? mPIItr->get() : nullptr;
  }

  bool operator==(const ParallelItemRef &RHS) const {
    return mState == RHS.mState &&
           (!isValid() || getUnchecked() == RHS.getUnchecked());
  }

  bool operator!=(const ParallelItemRef &RHS) const { return operator==(RHS); }

private:
  friend class llvm::DenseMapInfo<ParallelItemRef>;

  static ParallelItemRef getEmptyRef() { return ParallelItemRef{}; }
  static ParallelItemRef getTombstoneRef() {
    ParallelItemRef Ref;
    Ref.mState |= Tombstone;
    return Ref;
  }

  Parallelization::iterator mPLocListItr;
  Parallelization::location_iterator mPLocItr;
  ParallelBlock::iterator mPIItr;
  State mState{Empty};
};

template<typename ItemT, typename AnchorT, typename... Ts>
std::pair<ParallelItemRef, bool> Parallelization::try_emplace(
    llvm::BasicBlock *BB, AnchorT Anchor, bool OnEntry, Ts &&... Args) {
  auto [PLocListItr, IsNew] = try_emplace(BB);
  auto PLocItr =
      llvm::find_if(PLocListItr->template get<ParallelLocation>(),
                    [Anchor](ParallelLocation &PL) {
                      return PL.Anchor.getOpaqueValue() == Anchor;
                    });
  if (PLocItr == PLocListItr->template get<ParallelLocation>().end()) {
    IsNew = true;
    PLocListItr->template  get<ParallelLocation>().emplace_back();
    PLocItr = std::prev(PLocListItr->template get<ParallelLocation>().end());
    PLocItr->Anchor = Anchor;
  }
  auto &PB = OnEntry ? PLocItr->Entry : PLocItr->Exit;
  auto PIItr{PB.begin()};
  for (auto PIItrE{PB.end()}; PIItr != PIItrE; ++PIItr)
    if (auto ItemPI{llvm::dyn_cast<ItemT>(PIItr->get())};
        ItemPI && ItemPI->isMergeableWith(std::forward<Ts>(Args)...))
      break;
  if (PIItr == PB.end()) {
    IsNew = true;
    PB.push_back(std::make_unique<ItemT>(std::forward<Ts>(Args)...));
    PIItr = std::prev(PB.end());
  }
  return std::pair(ParallelItemRef{PLocListItr, PLocItr, PIItr, OnEntry},
                   IsNew);
}

template<typename ItemT, typename AnchorT, typename... Ts>
ParallelItemRef Parallelization::emplace(
    llvm::BasicBlock *BB, AnchorT Anchor, bool OnEntry, Ts &&... Args) {
  auto PLocListItr{try_emplace(BB).first};
  auto PLocItr =
      llvm::find_if(PLocListItr->template get<ParallelLocation>(),
                    [Anchor](ParallelLocation &PL) {
                      return PL.Anchor.getOpaqueValue() == Anchor;
                    });
  if (PLocItr == PLocListItr->template get<ParallelLocation>().end()) {
    PLocListItr->template  get<ParallelLocation>().emplace_back();
    PLocItr = std::prev(PLocListItr->template get<ParallelLocation>().end());
    PLocItr->Anchor = Anchor;
  }
  auto &PB = OnEntry ? PLocItr->Entry : PLocItr->Exit;
  PB.push_back(std::make_unique<ItemT>(std::forward<Ts>(Args)...));
  auto PIItr{std::prev(PB.end())};
  return ParallelItemRef{PLocListItr, PLocItr, PIItr, OnEntry};
}

template <typename ItemT, typename AnchorT>
ParallelItemRef Parallelization::find(llvm::BasicBlock *BB, AnchorT Anchor,
                                      bool OnEntry) {
  auto PLocListItr = find(BB);
  if (PLocListItr == end())
    return ParallelItemRef{};
  auto PLocItr =
      llvm::find_if(PLocListItr->template get<ParallelLocation>(),
                    [Anchor](ParallelLocation &PL) {
                      return PL.Anchor.getOpaqueValue() == Anchor;
                    });
  if (PLocItr == PLocListItr->template get<ParallelLocation>().end())
    return ParallelItemRef{};
  auto &PB = OnEntry ? PLocItr->Entry : PLocItr->Exit;
  auto PIItr = llvm::find_if(PB, [](auto &PI) {
    return llvm::isa<ItemT>(PI.get());
  });
  return PIItr != PB.end()
             ? ParallelItemRef{PLocListItr, PLocItr, PIItr, OnEntry}
             : ParallelItemRef{};
}
}

namespace llvm {
template <>
struct ValueIsPresent<tsar::ParallelItemRef> {
  using UnwrappedType = tsar::ParallelItem *;
  static inline bool isPresent(const tsar::ParallelItemRef &Ref) {
    return Ref.isValid();
  }
  static inline decltype(auto) unwrapValue(tsar::ParallelItemRef &Ref) {
    return Ref.getUnchecked();
  }
};

template <>
struct ValueIsPresent<const tsar::ParallelItemRef> {
  using UnwrappedType = const tsar::ParallelItem *;
  static inline bool isPresent(const tsar::ParallelItemRef &Ref) {
    return Ref.isValid();
  }
  static inline decltype(auto) unwrapValue(const tsar::ParallelItemRef &Ref) {
    return Ref.getUnchecked();
  }
};

template <typename To> struct CastIsPossible<To, const tsar::ParallelItemRef> {
  static inline bool isPossible(const tsar::ParallelItemRef &Ref) {
    return CastIsPossible<To, const tsar::ParallelItem *>::isPossible(
        Ref.getUnchecked());
  }
};

template <typename To> struct CastIsPossible<To, tsar::ParallelItemRef> {
  static inline bool isPossible(const tsar::ParallelItemRef &Ref) {
    return CastIsPossible<To, const tsar::ParallelItem *>::isPossible(
        Ref.getUnchecked());
  }
};

template <typename To>
struct CastInfo<To, tsar::ParallelItemRef>
    : public CastIsPossible<To, tsar::ParallelItemRef> {
  using CastReturnType =
      typename cast_retty<To, tsar::ParallelItem *>::ret_type;
  static inline CastReturnType doCast(tsar::ParallelItemRef &Ref) {
    return CastInfo<To, tsar::ParallelItem *>::doCast(Ref.getUnchecked());
  }
  static inline CastReturnType castFailed() { return CastReturnType{}; }
  static inline CastReturnType doCastIfPossible(tsar::ParallelItemRef &Ref) {
    if (!CastInfo<To, tsar::ParallelItemRef>::isPossible(Ref))
      return castFailed();
    return doCast(Ref);
  }
};

template <typename To>
struct CastInfo<To, const tsar::ParallelItemRef>
    : public CastIsPossible<To, const tsar::ParallelItemRef> {
  using CastReturnType =
      typename cast_retty<To, const tsar::ParallelItem *>::ret_type;
  static inline CastReturnType doCast(const tsar::ParallelItemRef &Ref) {
    return CastInfo<To, const tsar::ParallelItem *>::doCast(Ref.getUnchecked());
  }
  static inline CastReturnType castFailed() { return CastReturnType{}; }
  static inline CastReturnType
  doCastIfPossible(const tsar::ParallelItemRef &Ref) {
    if (!CastInfo<To, const tsar::ParallelItemRef>::isPossible(Ref))
      return castFailed();
    return doCast(Ref);
  }
};

template <> struct DenseMapInfo<tsar::ParallelItemRef> {
  static inline tsar::ParallelItemRef getEmptyKey() {
    return tsar::ParallelItemRef::getEmptyRef();
  }
  static inline tsar::ParallelItemRef getTombstoneKey() {
    return tsar::ParallelItemRef::getTombstoneRef();
  }
  static inline unsigned getHashValue(const tsar::ParallelItemRef &Ref) {
    assert(Ref && "Reference must be valid!");
    return DenseMapInfo<tsar::ParallelItem *>::getHashValue(Ref.getUnchecked());
  }
  static inline unsigned getHashValue(const tsar::ParallelItem *PI) {
    return DenseMapInfo<tsar::ParallelItem *>::getHashValue(PI);
  }
  static inline bool isEqual(const tsar::ParallelItemRef &LHS,
                             const tsar::ParallelItemRef &RHS) {
    return LHS == RHS;
  }
  static inline bool isEqual(const tsar::ParallelItem *LHS,
                             const tsar::ParallelItemRef &RHS) {
    return RHS && LHS == RHS.getUnchecked();
  }
};
}
#endif//TSAR_PARALLELIZATION_H
