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
// This file implements functions from Directives.h which allow to process
// TSAR directives.
//
//===----------------------------------------------------------------------===//

#include "tsar/Support/Directives.h"

using namespace llvm;
using namespace tsar;

/// Table of string directive names indexed by enum value.
static const char * const NamespaceNameTable[] = {
  "not_namespace",
#define GET_NAMESPACE_NAME_TABLE
#include "tsar/Support/Directives.gen"
#undef GET_NAMESPACE_NAME_TABLE
};

/// Table of string directive names indexed by enum value.
static const char * const DirectiveNameTable[] = {
  "not_directive",
#define DIRECTIVE(ID, Name, HasBody) Name,
#define GET_DIRECTIVE_LIST
#include "tsar/Support/Directives.gen"
#undef GET_DIRECTIVE_LIST
#undef DIRECTIVE
};

static constexpr bool DirectiveBodyTable[] = {
  false,
#define DIRECTIVE(ID, Name, HasBody) HasBody,
#define GET_DIRECTIVE_LIST
#include "tsar/Support/Directives.gen"
#undef GET_DIRECTIVE_LIST
#undef DIRECTIVE
};

/// Table of string clause names indexed by enum value.
static const char * const ClauseNameTable[] = {
  "not_clause",
#define GET_CLAUSE_NAME_TABLE
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_NAME_TABLE
};

static const char * const ClauseExprNameTable[] = {
  "not_expression",
#define KIND(EK, IsSingle, ClangTok) #EK,
#define GET_CLAUSE_EXPR_KINDS
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_EXPR_KINDS
#undef KIND
};

static constexpr bool ClauseExprSingleTable[] = {
  false,
#define KIND(EK, IsSingle, ClangTok) IsSingle,
#define GET_CLAUSE_EXPR_KINDS
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_EXPR_KINDS
#undef KIND
};

static constexpr DirectiveNamespaceId DirectiveNamespaceTable[] = {
#define NAMESPACE(Id) DirectiveNamespaceId::Id,
  NAMESPACE(NotNamespace) // there is no namespace for invalid directive
#define GET_DIRECTIVE_NAMESPACE_TABLE
#include "tsar/Support/Directives.gen"
#undef GET_DIRECTIVE_NAMESPACE_TABLE
#undef NAMESPACE
};

static constexpr DirectiveId ClauseDirectiveTable[] = {
#define DIRECTIVE(Id) DirectiveId::Id,
  DIRECTIVE(NotDirective) // there is no directive for invalid clause
#define GET_CLAUSE_DIRECTIVE_TABLE
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_DIRECTIVE_TABLE
#undef DIRECTIVE
};

/// This literal type contains offsets for a prototype in table of prototypes
/// (see PrototypeOffsetTable and PrototypeTable for details).
namespace {
struct PrototypeDescriptor {
  unsigned Start;
  unsigned End;
};
}

///\brief Table of offsets in prototype table indexed by enum values.
///
/// Each clause has a prototype which is described by a table of prototypes.
/// Each description record has start and end points which are stored in this
/// table. So start point of a prototype for an clase ID can be accessed
/// in a following way `PrototypeTable[PrototypeOffsetTable[Id].first]`.
static constexpr PrototypeDescriptor PrototypeOffsetTable[] = {
#define PROTOTYPE(Start,End) {Start, End},
  PROTOTYPE(0,0) // there is no prototype for invalid clause
#define GET_CLAUSE_PROTOTYPE_OFFSET_TABLE
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_PROTOTYPE_OFFSET_TABLE
#undef PROTOTYPE
};

/// Table of prototypes of clauses indexed by records in prototype offset table.
static constexpr ClauseExpr PrototypeTable[] = {
#define CLAUSE_EXPR(EK) ClauseExpr::EK,
#define GET_CLAUSE_PROTOTYPE_TABLE
#include "tsar/Support/Directives.gen"
#undef GET_CLAUSE_PROTOTYPE_TABLE
#undef CLAUSE_EXPR
};

namespace tsar {
StringRef getName(DirectiveNamespaceId Id) noexcept {
  assert(Id < DirectiveNamespaceId::NumNamespaces &&
    Id > DirectiveNamespaceId::NotNamespace && "Invalid namepsace ID!");
  return NamespaceNameTable[static_cast<unsigned>(Id)];
}

StringRef getName(DirectiveId Id) noexcept {
  assert(Id < DirectiveId::NumDirectives && Id > DirectiveId::NotDirective &&
    "Invalid directive ID!");
  return DirectiveNameTable[static_cast<unsigned>(Id)];
}

StringRef getName(ClauseId Id) noexcept {
  assert(Id < ClauseId::NumClauses && Id > ClauseId::NotClause &&
    "Invalid clause ID!");
  return ClauseNameTable[static_cast<unsigned>(Id)];
}

StringRef getName(ClauseExpr EK) noexcept {
  assert(EK < ClauseExpr::NumExprs && EK > ClauseExpr::NotExpr &&
    "Invalid clause expression kind!");
  return ClauseExprNameTable[static_cast<unsigned>(EK)];
}

bool hasBody(DirectiveId Id) noexcept {
  assert(Id < DirectiveId::NumDirectives && Id > DirectiveId::NotDirective &&
    "Invalid directive ID!");
  return DirectiveBodyTable[static_cast<unsigned>(Id)];
}

bool isSingle(ClauseExpr EK) noexcept {
  assert(EK < ClauseExpr::NumExprs && EK > ClauseExpr::NotExpr &&
    "Invalid clause expression kind!");
  return ClauseExprSingleTable[static_cast<unsigned>(EK)];
}

DirectiveNamespaceId getParent(DirectiveId Id) noexcept {
  assert(Id < DirectiveId::NumDirectives && Id > DirectiveId::NotDirective &&
    "Invalid directive ID!");
  return DirectiveNamespaceTable[static_cast<unsigned>(Id)];
}

DirectiveId getParent(ClauseId Id) noexcept {
  assert(Id < ClauseId::NumClauses && Id > ClauseId::NotClause &&
    "Invalid clause ID!");
  return ClauseDirectiveTable[static_cast<unsigned>(Id)];
}

bool getTsarDirectiveNamespace(StringRef Name, DirectiveNamespaceId &Id) {
  const char* const *Start = &NamespaceNameTable[0];
  const char* const *End =
    &NamespaceNameTable[(unsigned)DirectiveNamespaceId::NumNamespaces];
  if (Name.empty())
    return false;
  const char* const *I = std::find(Start, End, Name);
  if (I != End) {
    Id = (DirectiveNamespaceId)(I - Start);
    return true;
  }
  return false;
}

bool getTsarDirective(DirectiveNamespaceId Namespace, StringRef Name,
    DirectiveId &Id) {
  const char* const *Start = &DirectiveNameTable[0];
  const char* const *End =
    &DirectiveNameTable[(unsigned)DirectiveId::NumDirectives];
  if (Name.empty())
    return false;
  const char* const *I = std::find_if(Start, End,
    [&Name, &Start, &Namespace](const char * const &D) {
      return Name == D && getParent((DirectiveId)(&D - Start)) == Namespace; });
  if (I != End) {
    Id = (DirectiveId)(I - Start);
    return true;
  }
  return false;
}

bool getTsarClause(DirectiveId Directive, StringRef Name, ClauseId &Id) {
  const char* const *Start = &ClauseNameTable[0];
  const char* const *End =
    &ClauseNameTable[(unsigned)ClauseId::NumClauses];
  const char* const *I = std::find_if(Start, End,
    [&Name, &Start, &Directive](const char * const &C) {
      return Name == C && getParent((ClauseId)(&C - Start)) == Directive; });
  if (I != End) {
    Id = (ClauseId)(I - Start);
    return true;
  }
  return false;
}

ClausePrototype ClausePrototype::get(ClauseId Id) noexcept {
  assert(Id < ClauseId::NumClauses && Id > ClauseId::NotClause &&
    "Invalid clause ID!");
  auto Offset = PrototypeOffsetTable[static_cast<unsigned>(Id)];
  return ClausePrototype(Id,
    &PrototypeTable[Offset.Start], &PrototypeTable[Offset.End]);
}
}
