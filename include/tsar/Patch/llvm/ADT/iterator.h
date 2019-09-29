//===--- iterator.h ------- Patch Implementation ----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains fixed implementation for some of iterator classes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PATCH_LLVM_ADT_ITERATOR_H
#define TSAR_PATCH_LLVM_ADT_ITERATOR_H

#include <llvm/ADT/iterator.h>

namespace llvm {
namespace patch {
template <typename WrappedIteratorT,
          typename T = decltype(&*std::declval<WrappedIteratorT>())>
class pointer_iterator
// PATCH: specialization of iterator base has been fixed.
      : public iterator_adaptor_base<
        pointer_iterator<WrappedIteratorT>,
        WrappedIteratorT,
        typename std::iterator_traits<WrappedIteratorT>::iterator_category,
        T> {

  mutable T Ptr;

public:
  pointer_iterator() = default;

  explicit pointer_iterator(WrappedIteratorT u)
      : pointer_iterator::iterator_adaptor_base(std::move(u)) {}

  T &operator*() { return Ptr = &*this->I; }
  const T &operator*() const { return Ptr = &*this->I; }
};
}
}

#endif//TSAR_PATCH_LLVM_ADT_ITERATOR_H
