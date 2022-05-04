//===----- Utils.h -------- Utility Methods and Classes ---------*- C++ -*-===//
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
// This file defines abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SUPPORT_UTILS_H
#define TSAR_SUPPORT_UTILS_H

#include "tsar/Core/tsar-config.h"
#include <llvm/ADT/StringRef.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>

namespace llvm::sys::fs {
class TempFile;
}

#if !defined LLVM_RELEASE_BUILD && defined TSAR_ENABLE_LLVM_DUMP
/// Use this macro if dump() is called for LLVM objects. Otherwise, link-time
/// errors occur if configuration of LLVM build is not Debug.
# define TSAR_LLVM_DUMP(X) do { X; } while (false)
#else
/// Use this macro if dump() is called for LLVM objects. Otherwise, link-time
/// errors occur if configuration of LLVM build is not Debug.
# define TSAR_LLVM_DUMP(X)
#endif

namespace tsar {
/// Return identifier of a specified pass and destroy the pass.
inline llvm::AnalysisID getPassIDAndErase(llvm::Pass *P) {
  auto ID = P->getPassID();
  delete P;
  return ID;
}

/// Merges elements from a specified range using a specified delimiter, put
/// result to a specified buffer and returns reference to it.
template<class ItrT>
llvm::StringRef join(ItrT I, ItrT EI, llvm::StringRef Delimiter,
    llvm::SmallVectorImpl<char> &Out) {
  llvm::raw_svector_ostream OS(Out);
  OS << *I;
  for (++I; I != EI; ++I)
    OS <<  Delimiter << *I;
  return llvm::StringRef(Out.data(), Out.size());
}

/// Merges elements from a specified range using a specified delimiter.
template<class ItrT>
std::string join(ItrT I, ItrT EI, llvm::StringRef Delimiter) {
  llvm::SmallString<256> Out;
  return join(I, EI, Delimiter, Out);
}

/// \brief Splits a specified string into tokens according to a specified
/// pattern.
///
/// See std::regex for syntax of a pattern string. If there are successful
/// sub-matches, each of them becomes a token. Otherwise, the whole matching
/// sequence becomes a token.
/// For example, in case of `(struct)\s` pattern and `struct A` string, `struct`
/// is a token (the whole matching sequence is `struct `). However, in case of
/// `struct\s` pattern `strcut ` is a token.
///
/// \attention Note, that this function does not allocate memory for the result.
/// This means that result vector contains references to substrings of
/// a specified string `Str` and the vector will be valid only while
/// `Str` is valid.
std::vector<llvm::StringRef> tokenize(
  llvm::StringRef Str, llvm::StringRef Pattern);

/// \brief An iterator type that allows iterating over the pointers via some
/// other iterator.
///
/// The typical usage of this is to expose a type that iterates over Ts, but
/// which is implemented with some iterator over some wrapper of T*s
/// \code
///   typedef wrapped_pointer_iterator<
///     std::vector<std::unique_ptr<T>>::iterator> iterator;
/// \endcode
template <class WrappedIteratorT,
          class T = decltype(&**std::declval<WrappedIteratorT>())>
class wrapped_pointer_iterator : public llvm::iterator_adaptor_base<
  wrapped_pointer_iterator<WrappedIteratorT>, WrappedIteratorT,
    class std::iterator_traits<WrappedIteratorT>::iterator_category, T> {
public:
  wrapped_pointer_iterator() = default;
  template<class ItrT> wrapped_pointer_iterator(ItrT &&I) :
    wrapped_pointer_iterator::iterator_adaptor_base(std::forward<ItrT &&>(I)) {}

  T & operator*() { return Ptr = &**this->I; }
  const T & operator*() const { return Ptr = &**this->I; }

private:
  mutable T Ptr;
};

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

}
#endif // TSAR_SUPPORT_UTILS_H
