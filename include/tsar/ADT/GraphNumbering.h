//===- GraphNumbering.h -- Graph Node Numbering Helper-----------*- C++ -*-===//
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
// This file defines helper classes and functions to calculate preorder and
// postorder numberings of nodes in a graph. Forward and backward numberings
// are performed.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_GRAPH_NUMBERING_H
#define TSAR_GRAPH_NUMBERING_H

#include "tsar/ADT/DenseMapTraits.h"
#include <bcl/tagged.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>

//===----------------------------------------------------------------------===//
// Tags which represent supported numberings of graph nodes.
//===----------------------------------------------------------------------===//
struct Preorder {};
struct ReversePreorder {};
struct Postorder {};
struct ReversePostorder {};

/// \brief Numbering traits.
///
/// \tparam NodeRef Type of Node token in the graph, which should
/// be cheap to copy.
/// \tparam NumberingRef Type of storage of numbers of all node in a graph,
/// which should be cheap to copy.
///
/// This class should be specialized by different numbering storages
/// which is why the default version is empty. The specialization is used
/// to access number of nodes.
/// The following elements should be provided:
/// - typedef NumberRef - Type of storage of node numbers, which should
///     be cheap to copy.
/// - static NumberRef get(NodeRef, const NumberingType &) - Returns storage of
///     numbers of a specified node.
/// - static void setPreorder(std::size_t, NumberRef),
///   static void setReversePreorder(std::size_t, NumberRef),
///   static void setPostorder(std::size_t, NumberRef),
///   static void setReversePostorder(std::size_t, NumberRef) - This functions
///     store appropriate number into a specified storage.
namespace tsar {
template<class NodeRef, class NumberingRef> struct NumberingTraits {
  typedef typename NumberingRef::UnknownNumberingError NumberRef;
};

/// This is a default storage of numbers of graph nodes.
using NodeNumbering = bcl::tagged_tuple<
  bcl::tagged<std::size_t, Preorder>,
  bcl::tagged<std::size_t, ReversePreorder>,
  bcl::tagged<std::size_t, Postorder>,
  bcl::tagged<std::size_t, ReversePostorder>>;

/// This is a default storage (map for node to numbers) of numbers
/// of all graph nodes.
template<class NodeRef>
using GraphNumbering = llvm::DenseMap<
  NodeRef,
  std::tuple<
    std::size_t, std::size_t,
    std::size_t, std::size_t>,
  llvm::DenseMapInfo<NodeRef>,
  TaggedDenseMapTuple<
    bcl::tagged<NodeRef, typename std::decay<NodeRef>::type>,
    bcl::tagged<std::size_t, Preorder>,
    bcl::tagged<std::size_t, ReversePreorder>,
    bcl::tagged<std::size_t, Postorder>,
    bcl::tagged<std::size_t, ReversePostorder>>>;

/// This is a default numbering traits.
template<class NodeRef>
struct NumberingTraits<NodeRef, GraphNumbering<NodeRef> *> {
  using NumberRef = typename GraphNumbering<NodeRef>::iterator;
  static NumberRef get(NodeRef N, GraphNumbering<NodeRef> *GN) {
    using NumberingTuple = typename GraphNumbering<NodeRef>::mapped_type;
    return GN->insert(std::make_pair(N, NumberingTuple(0, 0, 0, 0))).first;
  }
  static void setPreorder(std::size_t N, NumberRef I) {
    I->template get<Preorder>() = N;
  }
  static void setReversePreorder(std::size_t N, NumberRef I) {
    I->template get<ReversePreorder>() = N;
  }
  static void setPostorder(std::size_t N, NumberRef I) {
    I->template get<Postorder>() = N;
  }
  static void setReversePostorder(std::size_t N, NumberRef I) {
    I->template get<ReversePostorder>() = N;
  }
};

/// \brief A private class which is used to figure out where to store
/// the visited set (for details, see llvm::df_iterator_storage).
template <class NodeRef, class NumberingRef,
  class NT = NumberingTraits<NodeRef, NumberingRef>, unsigned SmallSize = 8>
class DFIteratorNumberingSet : public llvm::SmallPtrSet<NodeRef, SmallSize> {
public:
  using BaseSet = llvm::SmallPtrSet<NodeRef, SmallSize>;
  using iterator = typename BaseSet::iterator;
  DFIteratorNumberingSet(NumberingRef Numbering, std::size_t GraphSize) :
      mNumbering(Numbering) {
    mLastNumber.get<Preorder>() = 1;
    mLastNumber.get<ReversePreorder>() = GraphSize;
    mLastNumber.get<Postorder>() = 1;
    mLastNumber.get<ReversePostorder>() = GraphSize;
  }
  std::pair<iterator, bool> insert(NodeRef N) {
    auto I = NT::get(N, mNumbering);
    NT::setPreorder(mLastNumber.get<Preorder>()++, I);
    NT::setReversePreorder(mLastNumber.get<ReversePreorder>()--, I);
    return BaseSet::insert(N);
  }
  void completed(NodeRef N) {
    auto I = NT::get(N, mNumbering);
    NT::setPostorder(mLastNumber.get<Postorder>()++, I);
    NT::setReversePostorder(mLastNumber.get<ReversePostorder>()--, I);
  }
private:
  NumberingRef mNumbering;
  NodeNumbering mLastNumber;
};

/// \brief This calculates numbers of graph nodes and store their into
/// a specified storage.
///
/// A SmallSize parameter is some internal parameter which sometimes can be used
/// for optimization. For details, see DFIteratorNumberingSet.
template<
  class GraphTy, class NumberingRef,
  class NT = NumberingTraits<
    typename llvm::GraphTraits<GraphTy>::NodeRef, NumberingRef>,
  unsigned SmallSize = 8>
void numberGraph(const GraphTy &G, NumberingRef N) {
  using namespace llvm;
  using GT = GraphTraits<GraphTy>;
  DFIteratorNumberingSet<
    typename GT::NodeRef, NumberingRef, NT, SmallSize> S(N, GT::size(G));
  for (auto I = df_ext_begin(G, S), E = df_ext_end(G, S); I != E; ++I);
}
}

#endif//TSAR_GRAPH_NUMBERING_H
