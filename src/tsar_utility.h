//===----- tsar_utility.h - Utility Methods and Classes ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//
#ifndef TSAR_UTILITY_H
#define TSAR_UTILITY_H

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/LoopInfo.h>
#include <tuple>
#include <tagged.h>

namespace llvm {
class DIGlobalVariable;
class GlobalVariable;
class DILocalVariable;
class AllocaInst;
}

namespace tsar {
/// This tag provides access to low-level representation of matched entities.
struct IR {};

/// This tag provides access to source-level representation of matched entities.
struct AST {};

/// Compares two set.
template<class PtrType, unsigned SmallSize>
bool operator==(const llvm::SmallPtrSet<PtrType, SmallSize> &LHS,
  const llvm::SmallPtrSet<PtrType, SmallSize> &RHS) {
  if (LHS.size() != RHS.size())
    return false;
  for (PtrType V : LHS)
    if (RHS.count(V) == 0)
      return false;
  return true;
}

/// Compares two set.
template<class PtrType, unsigned SmallSize>
bool operator!=(const llvm::SmallPtrSet<PtrType, SmallSize> &LHS,
  const llvm::SmallPtrSet<PtrType, SmallSize> &RHS) {
  return !(LHS == RHS);
}

namespace detail {
/// Applies a specified function object to each loop in a loop tree.
template<class Function>
void for_each(llvm::LoopInfo::reverse_iterator ReverseI,
  llvm::LoopInfo::reverse_iterator ReverseEI,
  Function F) {
  for (; ReverseI != ReverseEI; ++ReverseI) {
    F(*ReverseI);
    for_each((*ReverseI)->rbegin(), (*ReverseI)->rend(), F);
  }
}
}

/// Applies a specified function object to each loop in a loop tree.
template<class Function>
Function for_each(const llvm::LoopInfo &LI, Function F) {
  detail::for_each(LI.rbegin(), LI.rend(), F);
  return std::move(F);
}

/// Returns a meta information for a global variable or nullptr;
llvm::DIGlobalVariable * getMetadata(const llvm::GlobalVariable *Var);

/// Returns a meta information for a local variable or nullptr;
llvm::DILocalVariable * getMetadata(const llvm::AllocaInst *AI);

/// \brief This is an implementation of detail::DenseMapPair which supports
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

/// \brief This is an implementation of detail::DenseMapPair which supports
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
///     tsar::TaggedDenseMapPair<
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
    return BaseT::second.get<Tag>();
  }

  template<class Tag> const bcl::get_tagged_t<Tag, TaggedValue...> &
  get(std::false_type) const noexcept {
    return BaseT::second.get<Tag>();
  }
};
}

namespace llvm {
/// This allows passes to create callbacks that run when the underlying Value
/// has RAUW called on it or is destroyed.
struct  CallbackVHFactory {
  /// Invokes when someone wants to create callbacks for a specified value.
  virtual void create(llvm::Value *V) = 0;

  /// Invokes when someone wants to delete all created callbacks, for example
  /// in Pass::releaseMemory() method.
  virtual void destroy() = 0;

  /// Virtual destructor.
  virtual ~CallbackVHFactory() {}
};
}
#endif//TSAR_UTILITY_H
