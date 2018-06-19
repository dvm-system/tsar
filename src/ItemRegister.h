//===-- ItemRegister.h - Register of Distinct Items -------------*- C++ -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file define class to register some items. This class attaches an
// identifier to each item.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ITEM_REGISTER_H
#define TSAR_ITEM_REGISTER_H

#include "tsar_utility.h"
#include <cell.h>
#include <utility.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/Type.h>
#include <type_traits>

namespace tsar {
/// \brief This class registers items, so it is possible to attach some
/// information to these items.
///
/// This class attaches an identifier to each item.
/// \tparam Tys Types of items that should be registered. Note, that for all
/// items of all Tys continuous numbering will be used.
///
/// For example, instrumentation passes use this class to register LLVM IR
template<class... Tys>
class ItemRegister : private bcl::Uncopyable {
public:
  /// Identifier of an item.
  using IdTy = uint64_t;

private:
  template<class Ty> struct RegisterConstructor {
    using CellKey = bcl::StaticMapKey<
      llvm::DenseMap<Ty, IdTy, llvm::DenseMapInfo<Ty>,
        tsar::TaggedDenseMapPair<
          bcl::tagged<Ty, Ty>,
          bcl::tagged<IdTy, IdTy>>>>;
  };

  template<class Ty>
  using RegisterKey = typename RegisterConstructor<Ty>::CellKey;

  template<class Ty>
  using Register = typename RegisterKey<Ty>::ValueType;

  /// This is a map from Ty to a Register which associates IDs with items
  /// of the type Ty.
  ///
  /// For example, Ty may be `const llvm::Type *` and Register will be
  /// a llvm::DenseMap from Ty to IdTy.
  using RegisterMap =  typename bcl::StaticMapConstructor<
    RegisterConstructor, Tys...>::Type;

  struct ClearFunctor {
    template<class CellTy> void operator()(CellTy *C) {
      using CellKey = typename CellTy::CellKey;
      C->template value<CellKey>().clear();
    }
  };

public:
  /// Returns number of possible item types.
  static IdTy numberOfItemTypes() noexcept { return sizeof...(Tys); }

  /// Returns index of a specified type in the list of item types.
  template<class Ty>
  static IdTy indexOfItemType() noexcept { return bcl::index_of<Ty, Tys...>(); }

  /// Creates a register. The first item will have a specified ID.
  explicit ItemRegister(IdTy FirstId = 0) noexcept : mIdNum(FirstId) {}

  /// Registers a new item if it has not been registered yet and
  /// returns its ID.
  template<class Ty>
  IdTy regItem(const Ty &Item) {
    auto Pair =
      mRegisters.template value<RegisterKey<Ty>>().try_emplace(Item, mIdNum);
    if (Pair.second)
      ++mIdNum;
    return Pair.first->template get<IdTy>();
  }

  /// Returns number of items that have been registered.
  IdTy numberOfIDs() const noexcept { return mIdNum; }

  /// Returns all items of a specified type Ty that have been registered.
  template<class Ty>
  const Register<Ty> & getRegister() const noexcept {
    return mRegisters.template value<RegisterKey<Ty>>();
  }

  /// Returns iterator that points to a item,id pair if a specified item has
  /// been registered.
  template<class Ty>
  typename Register<Ty>::const_iterator findItem(const Ty &Item) const {
    return getRegister<Ty>().find(Item);
  }

  /// Returns ID for a specified item. The item must be already registered.
  template<class Ty>
  IdTy getID(const Ty &Item) const {
    assert(getRegister<Ty>().count(Item) && "Item must be registered!");
    return getRegister<Ty>().find(Item)->template get<IdTy>();
  }

  /// Returns ID for a specified item. The item must be already registered.
  template<class Ty>
  IdTy operator[](const Ty &Item) const { return getID(Item); }

  /// Removes all registered items and sets the first available ID to `FirstId`.
  void clear(IdTy FirstId = 0) {
    mRegisters.for_each(ClearFunctor());
    mIdNum = FirstId;
  }

private:
  /// Number of used IDs.
  IdTy mIdNum = 0;
  RegisterMap mRegisters;
};
}

#endif//TSAR_ITEM_REGISTER_H
