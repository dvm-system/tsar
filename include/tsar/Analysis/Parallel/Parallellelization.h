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
  explicit ParallelItem(unsigned Kind, bool IsFinal,
                        ParallelItem *Parent = nullptr)
    : ParallelItem(Kind, IsFinal, false, false, Parent) {}

  virtual ~ParallelItem();

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

  ParallelItem *getParent() noexcept { return mParent; }
  const ParallelItem *getParent() const noexcept { return mParent; }

  void setParent(ParallelItem *Parent) noexcept { mParent = Parent; }

  /// Mark item as final.
  ///
  /// \attention Overridden methods have to call this one to set corresponding
  /// flags.
  virtual void finalize() { mFlags |= Final; }

protected:
  ParallelItem(unsigned Kind, bool IsFinal, bool IsMarker, bool IsChildPossible,
               ParallelItem *Parent)
      : mParent(Parent), mFlags(NoProperty), mKind(Kind) {
    if (IsFinal)
      mFlags |= Final;
    if (IsMarker)
      mFlags |= Marker;
    if (IsChildPossible)
      mFlags |= ChildPossible;
  }

private:
  ParallelItem *mParent;
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

  explicit ParallelLevel(unsigned Kind, bool IsFinal,
                        ParallelItem *Parent = nullptr)
      : ParallelItem(Kind, IsFinal, false, true, Parent) {
  }

  ~ParallelLevel() {
    for (auto *Child : mChildren)
      Child->setParent(nullptr);
  }

  child_iterator child_insert(ParallelItem *Item) {
    if (Item->getParent()) {
      auto PrevChildItr = llvm::find(
          llvm::cast<ParallelLevel>(Item->getParent())->children(), Item);
      if (Item->getParent() != this) {
        assert(PrevChildItr !=
                   llvm::cast<ParallelLevel>(Item->getParent())->child_end() &&
               "Corrupted parallel item, parent must contain its child!");
        llvm::cast<ParallelLevel>(Item->getParent())->child_erase(PrevChildItr);
        mChildren.push_back(Item);
      } else if (PrevChildItr ==
                 llvm::cast<ParallelLevel>(Item->getParent())->child_end()) {
        mChildren.push_back(Item);
      }
    } else {
      mChildren.push_back(Item);
    }
    Item->setParent(this);
    return mChildren.end() - 1;
  }

  child_iterator child_erase(child_iterator I) {
    (*I)->setParent(nullptr);
    return mChildren.erase(I);
  }

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
    return Item->isMarker() && Item->getParent() &&
           llvm::isa<ItemT>(Item->getParent());
  }

  ParallelMarker(unsigned Kind, ItemT *For)
      : ParallelItem(Kind, true, true, false, For) {}
};

inline ParallelItem::~ParallelItem() {
  if (!isMarker())
    if (auto P = llvm::dyn_cast_or_null<ParallelLevel>(mParent)) {
      auto ChildItr = llvm::find(P->children(), this);
      assert(ChildItr != P->child_end() &&
             "Corrupted parallel item, parent must contain its child!");
      P->child_erase(ChildItr);
    }
}


/// Sequence which determines an order of parallel constructs in a source code.
/// This is similar to a basic block in a control-flow graph.
using ParallelBlock = llvm::SmallVector<std::unique_ptr<ParallelItem>, 4>;

/// This determine location in a source code to insert parallel constructs.
struct ParallelLocation {
  /// Source-code item which implies parallel constructs.
  llvm::PointerUnion<llvm::MDNode *, llvm::Instruction *> Anchor;

  /// Parallel constructs before a specified anchor.
  ParallelBlock Entry;

  /// Parallel constructs after a specified anchor.
  ParallelBlock Exit;
};

template<class ItemT> class ParallelItemRef;

/// This represents results of program parallelization.
class Parallelization {
  /// Collection of basic blocks with attached parallel blocks to them.
  using ParallelBlocks = llvm::DenseMap<
      llvm::BasicBlock *, llvm::SmallVector<ParallelLocation, 1>,
      llvm::DenseMapInfo<llvm::BasicBlock *>,
      TaggedDenseMapPair<bcl::tagged<llvm::BasicBlock *, llvm::BasicBlock>,
                         bcl::tagged<llvm::SmallVector<ParallelLocation, 1>,
                                     ParallelLocation>>>;

  /// Functions which contains parallel constructs.
  using ParallelFunctions = llvm::SmallPtrSet<llvm::Function *, 32>;

public:
  using iterator = ParallelBlocks::iterator;
  using const_iterator = ParallelBlocks::const_iterator;

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

  /// Look for a parallel item of a specified type 'ItemT'.
  template <typename ItemT, typename AnchorT>
  ParallelItemRef<ItemT> find(llvm::BasicBlock *BB, AnchorT Anchor,
                              bool OnEntry = true);

private:
  llvm::SmallPtrSet<llvm::Function *, 32> mParallelFuncs;
  ParallelBlocks mParallelBlocks;
};

/// This is a reference to a parallel item inside a parallelization.
template<class ItemT>
class ParallelItemRef {
  using ParallelLocationList = llvm::SmallVectorImpl<ParallelLocation>;
public:
  ParallelItemRef(Parallelization::iterator PLocListItr,
    ParallelLocationList::iterator PLocItr,
    ParallelBlock::iterator PIItr, bool OnEntry)
    : mPLocListItr(PLocListItr), mPLocItr(PLocItr), mPIItr(PIItr),
    mOnEntry(OnEntry), mIsValid(true) {}

  ParallelItemRef() = default;

  /// Return parallel item or nullptr if reference is invalid.
  ItemT * dyn_cast() const { return isValid() ? get() : nullptr; }

  /// Return parallel item.
  ItemT *get() const {
    assert(isValid() && "Reference is invalid!");
    return cast<ItemT>(mPIItr->get());
  }

  /// Return edge from basic block to a list of parallel locations.
  Parallelization::iterator getPE() const {
    assert(isValid() && "Reference is invalid!");
    return mPLocListItr;
  }

  /// Return parallel location which contains a parallel item.
  ParallelLocationList::iterator getPL() const {
    assert(isValid() && "Reference is invalid!");
    return mPLocItr;
  }

  /// Return parallel item
  ParallelBlock::iterator getPI() const {
    assert(isValid() && "Reference is invalid!");
    return mPIItr;
  }

  bool isOnEntry() const noexcept {
    assert(isValid() && "Reference is invalid!");
    return mOnEntry;
  }

  bool isValid() const noexcept { return mIsValid; }
  bool isInValid() const noexcept { return !isValid(); }
  operator bool() const noexcept { return isValid(); }

  explicit operator ItemT *() const { return get(); }

private:
  Parallelization::iterator mPLocListItr;
  ParallelLocationList::iterator mPLocItr;
  ParallelBlock::iterator mPIItr;
  bool mOnEntry;
  bool mIsValid = false;
};

template <typename ItemT, typename AnchorT>
ParallelItemRef<ItemT> Parallelization::find(
    llvm::BasicBlock *BB, AnchorT Anchor, bool OnEntry) {
  auto PLocListItr = find(BB);
  if (PLocListItr == end())
    return ParallelItemRef<ItemT>{};
  auto PLocItr =
      llvm::find_if(PLocListItr->template get<ParallelLocation>(),
                    [Anchor](ParallelLocation &PL) {
                      return PL.Anchor.is<std::decay_t<AnchorT>>() &&
                             PL.Anchor.get<std::decay_t<AnchorT>>() == Anchor;
                    });
  if (PLocItr == PLocListItr->template get<ParallelLocation>().end())
    return ParallelItemRef<ItemT>{};
  auto &PB = OnEntry ? PLocItr->Entry : PLocItr->Exit;
  auto PIItr = llvm::find_if(PB, [](auto &PI) { return isa<ItemT>(PI.get()); });
  return PIItr != PB.end()
             ? ParallelItemRef<ItemT>{PLocListItr, PLocItr, PIItr, OnEntry}
             : ParallelItemRef<ItemT>{};
}
}
#endif//TSAR_PARALLELIZATION_H
