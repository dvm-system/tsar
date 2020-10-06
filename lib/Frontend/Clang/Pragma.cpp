//===- Pragma.cpp ---------- Pragma Frontendr ----------------------*- C++ -*-===//
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
// This file implements SAPFOR specific #pragma handlers.
//
//===----------------------------------------------------------------------===//

#include "tsar/Frontend/Clang/Pragma.h"
#include "tsar/Frontend/Clang/ASTImportInfo.h"
#include "tsar/Support/Clang/Utils.h"
#include <clang/AST/Expr.h>
#include <clang/Lex/Lexer.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <type_traits>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace {
std::pair<StringRef, CompoundStmt::body_iterator>
traversePragmaName(Stmt &S) {
  auto CS = dyn_cast<CompoundStmt>(&S);
  if (!CS)
    return std::make_pair(StringRef(), nullptr);
  auto CurrStmt = CS->body_begin();
  if (CurrStmt == CS->body_end())
    return std::make_pair(StringRef(), nullptr);
  auto Cast = dyn_cast<ImplicitCastExpr>(*CurrStmt);
  // In case of C there will be ImplicitCastExpr, however in case of C++ it
  // will be omitted.
  auto LiteralStmt = Cast ? *Cast->child_begin() : *CurrStmt;
  auto Literal = dyn_cast<clang::StringLiteral>(LiteralStmt);
  if (!Literal)
    return std::make_pair(StringRef(), nullptr);
  ++CurrStmt;
  return std::make_pair(Literal->getString(), CurrStmt);
}

}

namespace tsar {
Pragma::Pragma(Stmt &S) : mStmt(&S) {
  auto PragmaBody = traversePragmaName(S);
  if (!getTsarDirectiveNamespace(PragmaBody.first, mNamespaceId) ||
    PragmaBody.second == cast<CompoundStmt>(S).body_end())
    return;
  auto DirectiveBody = traversePragmaName(**PragmaBody.second);
  if (!getTsarDirective(mNamespaceId, DirectiveBody.first, mDirectiveId))
    return;
  mDirective = cast<CompoundStmt>(*PragmaBody.second);
  mClauseBegin = DirectiveBody.second;
}

Pragma::Clause Pragma::clause(clause_iterator I) {
  auto Tmp = traversePragmaName(**I);
  return Clause(Tmp.first, Tmp.second,
    Tmp.second ? cast<CompoundStmt>(*I)->body_end() : nullptr);
}

bool findClause(Pragma &P, ClauseId Id, SmallVectorImpl<Stmt *> &Clauses) {
  if (!P || P.getDirectiveId() != getParent(Id))
    return false;
  auto CSize = Clauses.size();
  for (auto CI = P.clause_begin(), CE = P.clause_end(); CI != CE; ++CI) {
    ClauseId CId;
    if (!getTsarClause(P.getDirectiveId(), Pragma::clause(CI).getName(), CId))
      continue;
    if (CId == Id)
      Clauses.push_back(*CI);
  }
  return CSize != Clauses.size();
}

std::pair<bool, PragmaFlags::Flags> pragmaRangeToRemove(const Pragma &P,
    const SmallVectorImpl<Stmt *> &Clauses,
    const SourceManager &SM, const LangOptions &LangOpts,
    const ASTImportInfo &ImportInfo,
    SmallVectorImpl<CharSourceRange> &ToRemove, PragmaFlags::Flags Ignore) {
  assert(P && "Pragma must be valid!");
  using FlagRawT = std::underlying_type<PragmaFlags::Flags>::type;
  auto IgnoreMask = std::numeric_limits<FlagRawT>::max() ^ Ignore;
  // It isn't safe to remove clauses in macro, so always prevent transformation.
  IgnoreMask |= PragmaFlags::IsInMacro;
  SourceLocation PStart = P.getNamespace()->getBeginLoc();
  SourceLocation PEnd = P.getNamespace()->getEndLoc();
  if (PStart.isInvalid())
    return { false, PragmaFlags::DefaultFlags };
  auto PStartDExp = SM.getDecomposedExpansionLoc(PStart);
  auto IncludeToFID = SM.getDecomposedIncludedLoc(PStartDExp.first).first;
  auto ErrFlags =
      IncludeToFID.isValid() && !ImportInfo.MainFiles.count(PStartDExp.first)
          ? PragmaFlags::IsInHeader
          : PragmaFlags::DefaultFlags;
  if (PStart.isFileID()) {
    if (ErrFlags & IgnoreMask)
      return { false, ErrFlags };
    if (Clauses.size() == P.clause_size())
      ToRemove.push_back(
        CharSourceRange::getTokenRange({getStartOfLine(PStart, SM), PEnd}));
    else
      for (auto C : Clauses)
        ToRemove.push_back(CharSourceRange::getTokenRange(C->getSourceRange()));
    return { true, PragmaFlags::DefaultFlags };
  }
  Token Tok;
  auto PStartExp = SM.getExpansionLoc(PStart);
  if (Lexer::getRawToken(PStartExp, Tok, SM, LangOpts) ||
      Tok.isNot(tok::raw_identifier) || Tok.getRawIdentifier() != "_Pragma")
    ErrFlags |= PragmaFlags::IsInMacro;
  if (ErrFlags & IgnoreMask)
    return { false, ErrFlags };
  if (Clauses.size() == P.clause_size()) {
    ToRemove.push_back(SM.getExpansionRange({ PStart, PEnd }));
  } else {
    // We obtain positions of pragma and clause in a scratch buffer and
    // calculates offset from the beginning of _Pragma(...) directive.
    // Then we add this offset to expansion location to obtain location of
    // a clause in the source code.
    auto PSpelling = SM.getSpellingLoc(PStart).getRawEncoding();
    for (auto C : Clauses) {
      auto CSpellingS = SM.getSpellingLoc(C->getBeginLoc()).getRawEncoding();
      auto CSpellingE = SM.getSpellingLoc(C->getEndLoc()).getRawEncoding();
      // Offset of clause start from `spf` in _Pragma("spf ...
      auto OffsetS = CSpellingS - PSpelling;
      // Offset of clause end from `spf` in _Pragma("spf ...
      auto OffsetE = CSpellingE - PSpelling;
      ToRemove.emplace_back(CharSourceRange::getTokenRange(
        { PStartExp.getLocWithOffset(OffsetS + 8),     // ' ' before clause name
          PStartExp.getLocWithOffset(OffsetE + 9) })); // end of clause
    }
  }
  return { true, PragmaFlags::DefaultFlags };
}

llvm::StringRef getPragmaText(DirectiveId Id, llvm::SmallVectorImpl<char> &Out,
    clang::PragmaIntroducerKind PIK) {
  assert(DirectiveId::NotDirective < Id && Id < DirectiveId::NumDirectives &&
    "Invalid identifier of a directive!");
  DirectiveNamespaceId NID = getParent(Id);
  llvm::raw_svector_ostream OS(Out);
  switch (PIK) {
  case PIK_HashPragma: OS << "#pragma "; break;
  case PIK__Pragma: OS << "_Pragma(\""; break;
  case PIK___pragma: OS << "__pragma(\""; break;
  }
  OS << getName(NID) << " " << getName(Id);
  switch (PIK) {
  case PIK__Pragma: case PIK___pragma: OS << "\")"; break;
  }
  OS << "\n";
  return StringRef(Out.data(), Out.size());
}


llvm::StringRef getPragmaText(ClauseId Id, llvm::SmallVectorImpl<char> &Out,
    clang::PragmaIntroducerKind PIK) {
  assert(ClauseId::NotClause < Id && Id < ClauseId::NumClauses &&
    "Invalid identifier of a clause!");
  DirectiveId DID = getParent(Id);
  DirectiveNamespaceId NID = getParent(DID);
  llvm::raw_svector_ostream OS(Out);
  switch (PIK) {
  case PIK_HashPragma: OS << "#pragma "; break;
  case PIK__Pragma: OS << "_Pragma(\""; break;
  case PIK___pragma: OS << "__pragma(\""; break;
  }
  OS << getName(NID) << " " << getName(DID) << " " << getName(Id);
  switch (PIK) {
  case PIK__Pragma: case PIK___pragma: OS << "\")"; break;
  }
  OS << "\n";
  return StringRef(Out.data(), Out.size());
}
}
