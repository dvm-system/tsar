//===- LoopGraphTraits.h ----- APC Loop Graph Traits ------------*- C++ -*-===//
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
// This file contains specialization of llvm::GraphTraits to traverse LoopGraph.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_APC_LOOP_GRAPH_TRAITS_H
#define TSAR_APC_LOOP_GRAPH_TRAITS_H

#include <apc/GraphLoop/graph_loops.h>
#include <llvm/ADT/GraphTraits.h>

namespace llvm {
template<> struct GraphTraits<::LoopGraph *> {
  using NodeRef = ::LoopGraph *;
  static NodeRef getEntryNode(NodeRef L) noexcept { return L; }
  using ChildIteratorType =
    decltype(std::declval<::LoopGraph>().children)::iterator;
  static ChildIteratorType child_begin(NodeRef L) {
    return L->children.begin();
  }
  static ChildIteratorType child_end(NodeRef L) {
    return L->children.end();
  }
};

template<> struct GraphTraits<const ::LoopGraph *> {
  using NodeRef = const ::LoopGraph *;
  static NodeRef getEntryNode(NodeRef L) noexcept { return L; }
  using ChildIteratorType =
    decltype(std::declval<::LoopGraph>().children)::const_iterator;
  static ChildIteratorType child_begin(NodeRef L) {
    return L->children.begin();
  }
  static ChildIteratorType child_end(NodeRef L) {
    return L->children.end();
  }
};
}
#endif//TSAR_APC_LOOP_GRAPH_TRAITS_H
