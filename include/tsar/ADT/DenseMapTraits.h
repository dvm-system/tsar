//===-- DenseMapTraits.h - DenseMap Usage Abstractions ----------*- C++ -*-===//
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
// This file provides specialization of some templates useful in case of
// llvm::DenseMap usage.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DENSE_MAP_TRAITS_H
#define TSAR_DENSE_MAP_TRAITS_H

#include <bcl/tagged.h>
#include <bcl/convertible_pair.h>
#include <llvm/ADT/DenseMapInfo.h>

namespace tsar {
/// This is an implementation of detail::DenseMapPair which supports
/// access to a first and second value via tag of a type (Pair.get<Tag>()).
///
/// Let us consider an example:
/// \code
///   struct Foo {};
///   typedef llvm::DenseMap<KT *, VT *, llvm::DenseMapInfo<KT *>,
///     tsar::TaggedDenseMapPair<
///       bcl::tagged<KT *, KT>, bcl::tagged<VT *, Foo>>> Map;
///   Map M;
///   auto I = M.begin();
///   I->get<KT>() // equivalent to I->first
///   I->get<Foo>() // equivalent to I->second
/// \endcode
template<class TaggedKey, class TaggedValue>
struct TaggedDenseMapPair : public bcl::tagged_pair<TaggedKey, TaggedValue> {
  typedef typename TaggedKey::type KeyT;
  typedef typename TaggedValue::type ValueT;
  KeyT &getFirst() { return std::pair<KeyT, ValueT>::first; }
  const KeyT &getFirst() const { return std::pair<KeyT, ValueT>::first; }
  ValueT &getSecond() { return std::pair<KeyT, ValueT>::second; }
  const ValueT &getSecond() const { return std::pair<KeyT, ValueT>::second; }
};

/// This is an implementation of detail::DenseMapPair which supports
/// access to all values via tag of a type.
///
/// It is possible to store in a map some tuple and access key and values from
/// this tuple via tag of a type.
/// Let us consider an example:
/// \code
///   struct Foo {};
///   struct Bar {};
///   typedef llvm::DenseMap<KT *,
///     std::tuple<VT1 *, VT2 *>,
///     llvm::DenseMapInfo<KT *>,
///     tsar::TaggedDenseMapTuple<
///       bcl::tagged<KT *, KT>,
///       bcl::tagged<VT1 *, Foo>>
///       bcl::tagged<VT2 *, Bar>>> Map;
///   Map M;
///   Map.insert(std::make_pair(K, std::make_tuple(V1, V2));
///   auto I = M.begin();
///   I->get<KT>() // equivalent to I->first
///   I->get<Foo>() // equivalent to I->second.get<0>();
///   I->get<Bar>() // equivalent to I->second.get<1>();
/// \endcode
template<class TaggedKey, class... TaggedValue>
struct TaggedDenseMapTuple :
    public std::pair<
      typename TaggedKey::type, bcl::tagged_tuple<TaggedValue...>> {
  typedef std::pair<
    typename TaggedKey::type, bcl::tagged_tuple<TaggedValue...>> BaseT;
  typedef typename TaggedKey::type KeyT;
  typedef std::tuple<typename TaggedValue::type...> ValueT;

  KeyT &getFirst() { return BaseT::first; }
  const KeyT &getFirst() const { return BaseT::first; }
  ValueT &getSecond() { return BaseT::second; }
  const ValueT &getSecond() const { return BaseT::second; }

  template<class Tag,
    class = typename std::enable_if<
      !std::is_void<
        bcl::get_tagged<Tag, TaggedKey, TaggedValue...>>::value>::type>
  bcl::get_tagged_t<Tag, TaggedKey, TaggedValue...> &
  get() noexcept {
    return get<Tag>(bcl::tags::is_alias<Tag, TaggedKey>());
  }

  template<class Tag,
    class = typename std::enable_if<
      !std::is_void<
        bcl::get_tagged<Tag, TaggedKey, TaggedValue...>>::value>::type>
  const bcl::get_tagged_t<Tag, TaggedKey, TaggedValue...> &
  get() const noexcept {
    return get<Tag>(bcl::tags::is_alias<Tag, TaggedKey>());
  }
private:
  template<class Tag>
  KeyT & get(std::true_type) noexcept { return BaseT::first; }

  template<class Tag>
  const KeyT & get(std::true_type) const noexcept { return BaseT::first; }

  template<class Tag> bcl::get_tagged_t<Tag, TaggedValue...> &
  get(std::false_type) noexcept {
    return BaseT::second.template get<Tag>();
  }

  template<class Tag> const bcl::get_tagged_t<Tag, TaggedValue...> &
  get(std::false_type) const noexcept {
    return BaseT::second.template get<Tag>();
  }
};
}

namespace llvm {
/// This is a specialization of llvm::DenseMapInfo for bcl::convertible_pair.
template<class FirstTy, class SecondTy>
class DenseMapInfo<bcl::convertible_pair<FirstTy, SecondTy>> :
  public DenseMapInfo<std::pair<FirstTy, SecondTy>> {};
}

#endif//TSAR_DENSE_MAP_TRAITS_H
