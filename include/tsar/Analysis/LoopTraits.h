//===------- LoopTraits.h ---- Loop Traits Template -------------*- C++ -*-===//
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
// This file defines the little LoopTraits<X> template class that should be
// specialized by classes which represent a loop tree and want to be iteratable
// by loops and blocks.
//
// This file proposes spcialization of this templat for llvm::Loop and
// <std::pair<llvm::Function *, llvm::LoopInfo *>.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_LOOP_TRAITS_H
#define TSAR_LOOP_TRAITS_H

#include <llvm/Analysis/LoopInfo.h>
#include <llvm/IR/Function.h>
#include <assert.h>
#include <utility>

namespace tsar {
/// This class should be specialized by different loops which is why
/// the default version is empty. The specialization is used to iterate over
/// blocks and internal loops which are part of a loop.
/// The following elements should be provided: typedef region_iterator,
/// - typedef block_iterator,
///   static block_iterator block_begin(LoopReptn &L),
///   static block_iterator block_end (LoopReptn &L) -
///     Allow iteration over all blocks in the specified loop.
/// - typedef loop_iterator,
///   static loop_iterator loop_begin(LoopReptn &L),
///   static loop_iterator loop_end (LoopReptn &L) -
///     Allow iteration over all internal loops in the specified loop.
template<class LoopReptn> class LoopTraits {
  /// If anyone tries to use this class without having an appropriate
  /// specialization, make an error.
  typedef typename LoopReptn::UnknownLoopError loop_iterator;
};

template<> class LoopTraits<llvm::Loop *> {
public:
  typedef llvm::Loop::block_iterator block_iterator;
  typedef llvm::Loop::iterator loop_iterator;
  static block_iterator block_begin(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->block_begin();
  }
  static block_iterator block_end(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->block_end();
  }
  static llvm::BasicBlock * getHeader(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->getHeader();
  }
  static loop_iterator loop_begin(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->begin();
  }
  static loop_iterator loop_end(llvm::Loop *L) {
    assert(L && "Loop must not be null!");
    return L->end();
  }
};

template<> class LoopTraits<std::pair<llvm::Function *, llvm::LoopInfo *>> {
  typedef std::pair<llvm::Function *, llvm::LoopInfo *> LoopReptn;
public:
  class block_iterator : public llvm::Function::iterator {
    // Let us use this iterator to access a list of blocks in a function
    // as a list of pointers.
    typedef llvm::Function::iterator base;
  public:
    explicit block_iterator(const llvm::Function::iterator &Itr) :
      llvm::Function::iterator(Itr) {}
    typedef pointer reference;
    block_iterator(reference R) : base(R) {}
    block_iterator() : base() {}
    reference operator*() const { return &base::operator *(); }
  };
  typedef llvm::LoopInfo::iterator loop_iterator;
  static block_iterator block_begin(LoopReptn L) {
    return block_iterator(L.first->begin());
  }
  static block_iterator block_end(LoopReptn L) {
    return block_iterator(L.first->end());
  }
  static llvm::BasicBlock * getHeader(LoopReptn L) {
    return &L.first->getEntryBlock();
  }
  static loop_iterator loop_begin(LoopReptn L) {
    return L.second->begin();
  }
  static loop_iterator loop_end(LoopReptn L) {
    return L.second->end();
  }
};
}
#endif//TSAR_LOOP_TRAITS_H