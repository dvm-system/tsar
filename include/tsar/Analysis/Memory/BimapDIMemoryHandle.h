//=== BimapDIMemoryHandle.h ---- Bimap Members Handle ---------- *- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
//===----------------------------------------------------------------------===//
//
// This file implements callback which run when the underlying DIMemory stored
// in a bimap has RAUW called on it or is destroyed.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_BIMAP_DI_MEMORY_HANDLE_H
#define TSAR_BIMAP_DI_MEMORY_HANDLE_H

#include "tsar/Analysis/Memory/DIMemoryHandle.h"
#include <llvm/Support/Debug.h>

#ifndef NDEBUG
# include "tsar/Unparse/Utils.h"
# include <llvm/BinaryFormat/Dwarf.h>
# undef DEBUG_TYPE
# define DEBUG_TYPE "di-memory-handle"
#endif

namespace tsar {
/// Callback which run when the underlying DIMemory stored in a bimap
/// has RAUW called on it or is destroyed.
template <class Tag, class BimapT>
class BimapDIMemoryHandle final : public CallbackDIMemoryHandle {
public:
  BimapDIMemoryHandle(DIMemory *M, BimapT *Map = nullptr)
      : CallbackDIMemoryHandle(M), mMap(Map) {}

  BimapDIMemoryHandle(const BimapDIMemoryHandle &) = default;
  BimapDIMemoryHandle(BimapDIMemoryHandle &&) = default;

  BimapDIMemoryHandle &operator=(const BimapDIMemoryHandle &) = default;
  BimapDIMemoryHandle &operator=(BimapDIMemoryHandle &&) = default;

  BimapDIMemoryHandle &operator=(DIMemory *M) {
    return *this = BimapDIMemoryHandle(M, mMap);
  }

  operator DIMemory *() const {
    return CallbackDIMemoryHandle::operator tsar::DIMemory *();
  }

private:
  void deleted() override {
    using namespace llvm;
    assert(mMap && "Container of memory must not be null!");
    auto I = mMap->template find<Tag>(getMemoryPtr());
    if (I != mMap->end()) {
      LLVM_DEBUG(
        dbgs() << "[DI MEMORY HANDLE]: delete memory location from the map ";
        printDILocationSource(dwarf::DW_LANG_C, *getMemoryPtr(), dbgs());
        dbgs() << "\n");
      mMap->erase(I);
    }
  }

  void allUsesReplacedWith(DIMemory *M) override {
    using namespace llvm;
    assert(M != getMemoryPtr() &&
           "Old and new memory locations must not be equal!");
    assert(mMap && "Container of memory must not be null!");
    assert((mMap->template find<Tag>(M) == mMap->end() ||
      mMap->template find<Tag>(M)->first != M) &&
      "New memory location is already presented in the map!");
    LLVM_DEBUG(dbgs() << "[DI MEMORY HANDLE]: replace memory location ";
               printDILocationSource(dwarf::DW_LANG_C, *getMemoryPtr(), dbgs());
               dbgs() << " with ";
               printDILocationSource(dwarf::DW_LANG_C, *M, dbgs());
               dbgs() << "\n");
    auto OldItr = mMap->template find<Tag>(getMemoryPtr());
    using MappedTag = typename std::conditional<
        bcl::tags::is_alias<
            typename BimapT::First,
            bcl::get_tagged<Tag, typename BimapT::value_type::taggeds>>::value,
        typename BimapT::Second, typename BimapT::First>::type;
    auto *Map = mMap;
    auto Mapped = OldItr->template get<MappedTag>();
    Map->erase(OldItr);
    emplaceFirstOrSecond(M, Map, std::move(Mapped),
                         std::is_same<typename BimapT::First, MappedTag>());
  }

  template <class MappedT>
  void emplaceFirstOrSecond(DIMemory *M, BimapT *Map, MappedT &&Mapped,
                            std::true_type) {
    Map->emplace(typename BimapT::value_type(
        std::piecewise_construct,
        std::forward_as_tuple(std::forward<MappedT>(Mapped)),
        std::forward_as_tuple(M, Map)));
  }

  template <class MappedT>
  void emplaceFirstOrSecond(DIMemory *M, BimapT *Map, MappedT &&Mapped,
                            std::false_type) {
    Map->emplace(typename BimapT::value_type(
        std::piecewise_construct, std::forward_as_tuple(M, Map),
        std::forward_as_tuple(std::forward<MappedT>(Mapped))));
  }

  BimapT *mMap;
};
} // namespace tsar

namespace llvm {
template <typename From> struct simplify_type;

// Specialize simplify_type to allow WeakDIMemoryHandle to participate in
// dyn_cast, isa, etc.
template <class Tag, class BimapT>
struct simplify_type<tsar::BimapDIMemoryHandle<Tag, BimapT>> {
  using SimpleType = tsar::DIMemory *;
  static SimpleType
  getSimplifiedValue(tsar::BimapDIMemoryHandle<Tag, BimapT> &MH) {
    return MH;
  }
};
template <class Tag, class BimapT>
struct simplify_type<const tsar::BimapDIMemoryHandle<Tag, BimapT>> {
  using SimpleType = tsar::DIMemory *;
  static SimpleType
  getSimplifiedValue(const tsar::BimapDIMemoryHandle<Tag, BimapT> &MH) {
    return MH;
  }
};
} // namespace llvm

#ifndef NDEBUG
# undef DEBUG_TYPE
#endif
#endif// TSAR_BIMAP_DI_MEMORY_HANDLE_H
