//===--- Directives.h ---- TSAR Directive Handling --------------*- C++ -*-===//
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
// This file defines a enum of directive identifiers supported by TSAR. Some
// helpful methods to process these directives are also defined.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DIRECTIVES_H
#define TSAR_DIRECTIVES_H

#include <llvm/ADT/StringRef.h>
#include <type_traits>

namespace tsar {
enum class DirectiveNamespaceId : unsigned {
  NotNamespace = 0,
#define GET_NAMESPACE_ENUM_VALUES
#include "tsar/Support/Directives.gen"
#undef GET_NAMESPACE_ENUM_VALUES
  NumNamespaces
};

/// This enum contains a value for every directive known by TSAR.
enum class DirectiveId : unsigned {
  NotDirective = 0,
#define DIRECTIVE(ID, Name, HasBody) ID,
#define GET_DIRECTIVE_LIST
#include "tsar/Support/Directives.gen"
#undef GET_DIRECTIVE_LIST
#undef DIRECTIVE
  NumDirectives
};

/// This enum contains a value for every clause known by TSAR.
enum class ClauseId : unsigned {
  NotClause = 0,
#define GET_CLAUSE_ENUM_VALUES
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_ENUM_VALUES
  NumClauses
};

/// Kinds of expressions which can be used to describe prototype of clauses.
enum class ClauseExpr : unsigned {
  NotExpr = 0,
#define KIND(EK, IsSignle, ClangTok) EK,
#define GET_CLAUSE_EXPR_KINDS
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_EXPR_KINDS
#undef KIND
  NumExprs
};

/// Returns the name for a specified namespace.
llvm::StringRef getName(DirectiveNamespaceId Id) noexcept;

/// Returns the name for a specified directive.
llvm::StringRef getName(DirectiveId Id) noexcept;

/// Returns the name for a specified clause.
llvm::StringRef getName(ClauseId Id) noexcept;

/// Returns string representation of expression kind.
llvm::StringRef getName(ClauseExpr EK) noexcept;

/// Return `true` if a specified directive has a body.
/// To represent the body prototype a clause with the empty name is used.
bool hasBody(DirectiveId Id) noexcept;

/// Returns `true` if a specified expression contains a single operand.
bool isSingle(ClauseExpr EK) noexcept;

/// Returns namespace for a specified directive.
DirectiveNamespaceId getParent(DirectiveId Id) noexcept;

/// Returns directive for a specified clause.
DirectiveId getParent(ClauseId Id) noexcept;

bool getTsarDirectiveNamespace(llvm::StringRef Name, DirectiveNamespaceId &Id);

/// Find tsar directive by name. Return false if not found.
bool getTsarDirective(DirectiveNamespaceId Namespace, llvm::StringRef Name,
  DirectiveId &Id);

/// Find tsar clause by name. Return false if not found.
bool getTsarClause(DirectiveId Directive, llvm::StringRef Name,
  ClauseId &Id);

/// This enables to iterate over expressions in clause prototype.
class ClausePrototype {
public:
  using iterator = const ClauseExpr *;
  using const_iterator = const ClauseExpr *;
  using size_type = std::size_t;

  static ClausePrototype get(ClauseId Id) noexcept;

  ClauseId getId() const noexcept { return mId; }

  const_iterator begin() const noexcept { return mBegin; }
  const_iterator end() const noexcept { return mEnd; }

  bool empty() noexcept { return mBegin == mEnd; }
  size_type size() noexcept { return std::distance(mBegin, mEnd); }

private:
  ClausePrototype(ClauseId Id, iterator I, iterator EI) :
      mId(Id), mBegin(I), mEnd(EI) {}

  ClauseId mId;
  const_iterator mBegin;
  const_iterator mEnd;
};

/// Inside a specified range finds anchor related to the complex expression
/// which is the nearest to the right of a specified iterator `I`.
inline ClausePrototype::const_iterator findAnchor(
    ClausePrototype::const_iterator I,
    ClausePrototype::const_iterator EI) noexcept {
  std::underlying_type<ClauseExpr>::type ComplexNum = 0;
  if (*I == ClauseExpr::EK_Anchor)
    return I;
  // Do not use isSingle() for the first value of I, because if it is a complex
  // expression ComplexNum will be increased and a corresponding anchor will
  // be lost.
  for (++I; I != EI; ++I) {
    if (!isSingle(*I))
      ++ComplexNum;
    if (*I == ClauseExpr::EK_Anchor)
      if (ComplexNum > 0)
        --ComplexNum;
      else
        return I;
  }
  return EI;
}

//===----------------------------------------------------------------------===//
// Overloaded arithmetic operations.
//===----------------------------------------------------------------------===//

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T, DirectiveNamespaceId>::value>::type>
inline T operator+(T Id, typename std::underlying_type<T>::type Add) noexcept {
  Id = static_cast<T>(
    static_cast<typename std::underlying_type<T>::type>(Id) + Add);
  return Id;
}

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T, DirectiveNamespaceId>::value>::type>
inline T operator-(T Id, typename std::underlying_type<T>::type Sub) noexcept {
  Id = static_cast<T>(
    static_cast<typename std::underlying_type<T>::type>(Id) - Sub);
  return Id;
}

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T, DirectiveNamespaceId>::value>::type>
inline T operator+(typename std::underlying_type<T>::type Add, T Id) noexcept {
  return Id + Add;
}

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T, DirectiveNamespaceId>::value>::type>
inline T & operator+=(T &Id,
    typename std::underlying_type<T>::type Add) noexcept {
  return Id = Id + Add;
}

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T, DirectiveNamespaceId>::value>::type>
inline T & operator++(T &Id) noexcept { return Id += 1; }

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T, DirectiveNamespaceId>::value>::type>
inline T operator++(T &Id, int) noexcept {
  auto Tmp = Id;
  ++Id;
  return Tmp;
}

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T, DirectiveNamespaceId>::value>::type>
inline T & operator-=(T &Id,
    typename std::underlying_type<T>::type Sub) noexcept {
  return Id = Id - Sub;
}

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T,DirectiveNamespaceId>::value>::type>
inline T & operator--(T &Id) noexcept { return Id -= 1;}

template<class T, class = typename std::enable_if<
  std::is_same<T, DirectiveId>::value || std::is_same<T, ClauseId>::value ||
  std::is_same<T, DirectiveNamespaceId>::value>::type>
inline T operator--(T &Id, int) noexcept {
  auto Tmp = Id;
  --Id;
  return Tmp;
}
}
#endif//TSAR_DIRECTIVES_H
