//===--- ClangUtils.cpp - Utilities To Examine Clang AST  -------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file provides utilities to examine Clang AST.
//
//===----------------------------------------------------------------------===//

#include "ClangUtils.h"
#include <clang/Analysis/CFG.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Lex/Lexer.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <numeric>

using namespace clang;
using namespace llvm;
using namespace tsar;

void tsar::unreachableBlocks(clang::CFG &Cfg,
    llvm::SmallPtrSetImpl<clang::CFGBlock *> &Blocks) {
  DenseSet<clang::CFGBlock *> ReachableBlocks;
  std::vector<clang::CFGBlock *> Worklist;
  Worklist.push_back(&Cfg.getEntry());
  ReachableBlocks.insert(&Cfg.getEntry());
  while (!Worklist.empty()) {
    auto Curr = Worklist.back();
    Worklist.pop_back();
    for (auto &Succ : Curr->succs()) {
      if (Succ.isReachable() &&
          ReachableBlocks.insert(Succ.getReachableBlock()).second)
        Worklist.push_back(Succ.getReachableBlock());
    }
  }
  for (auto *BB : Cfg)
    if (!ReachableBlocks.count(BB))
      Blocks.insert(BB);
}

LocalLexer::LocalLexer(SourceRange SR,
    const SourceManager &SM, const LangOptions &LangOpts) :
  mSR(SR), mSM(SM), mLangOpts(LangOpts) {
  mCurrentPos = SR.getBegin().getRawEncoding();
  auto SourceText =
    Lexer::getSourceText(CharSourceRange::getTokenRange(mSR), mSM, mLangOpts);
  mLength = mCurrentPos + (SourceText.empty() ? 0 : SourceText.size() - 1);
  assert(mLength <= std::numeric_limits<decltype(mCurrentPos)>::max() &&
    "Too long buffer!");
}

bool LocalLexer::LexFromRawLexer(clang::Token &Tok) {
  while (mCurrentPos <= mLength) {
    auto Loc = Lexer::GetBeginningOfToken(
      SourceLocation::getFromRawEncoding(mCurrentPos), mSM, mLangOpts);
    if (Lexer::getRawToken(Loc, Tok, mSM, mLangOpts, false)) {
      ++mCurrentPos;
      continue;
    }
    mCurrentPos += std::max(1u, (Tok.isAnnotation() ? 1u : Tok.getLength()));
    // Avoid duplicates for the same token.
    if (mTokens.empty() ||
        mTokens[mTokens.size() - 1].getLocation() != Tok.getLocation()) {
      mTokens.push_back(Tok);
      return false;
    }
  }
  return true;
}

std::vector<clang::Token> tsar::getRawIdentifiers(clang::SourceRange SR,
    const clang::SourceManager &SM, const clang::LangOptions &LangOpts) {
  LocalLexer::LexedTokens Tokens;
  LocalLexer Lex(SR, SM, LangOpts);
  Token Tok;
  while (!Lex.LexFromRawLexer(Tok)) {
    if (Tok.is(tok::raw_identifier))
      Tokens.push_back(Tok);
  }
  return Tokens;
}

void tsar::getRawMacrosAndIncludes(
    clang::FileID FID, const llvm::MemoryBuffer *InputBuffer,
    const clang::SourceManager &SM, const clang::LangOptions &LangOpts,
    llvm::StringMap<clang::SourceLocation> &Macros,
    llvm::StringMap<clang::SourceLocation> &Includes,
    llvm::StringSet<> &Ids) {
  Lexer L(FID, InputBuffer, SM, LangOpts);
  while (true) {
    Token Tok;
    L.LexFromRawLexer(Tok);
    if (Tok.is(tok::eof))
      break;
    if (!Tok.is(tok::hash) || !Tok.isAtStartOfLine()) {
      if (Tok.is(tok::raw_identifier))
        Ids.insert(Tok.getRawIdentifier());
      continue;
    }
    L.LexFromRawLexer(Tok);
    if (!Tok.is(tok::raw_identifier))
      continue;
    if (Tok.getRawIdentifier() == "define") {
      L.LexFromRawLexer(Tok);
      Macros.try_emplace(Tok.getRawIdentifier(), Tok.getLocation());
    } else if (Tok.getRawIdentifier() == "include") {
      L.LexFromRawLexer(Tok);
      auto Loc = Tok.getLocation();
      StringRef IncludeName;
      if (Tok.is(tok::less)) {
        do {
          L.LexFromRawLexer(Tok);
        } while (Tok.isNot(tok::greater));
        IncludeName = Lexer::getSourceText(
          CharSourceRange::getTokenRange(Loc, Tok.getLocation()),
          SM, LangOpts);
      } else {
        IncludeName = StringRef(Tok.getLiteralData(), Tok.getLength());
      }
      IncludeName = IncludeName.slice(1, IncludeName.size() - 1);
      Includes.try_emplace(IncludeName, Loc);
    }
  }
}

ExternalRewriter::ExternalRewriter(SourceRange SR, const SourceManager &SM,
    const LangOptions &LangOpts) : mSR(SR), mSM(SM), mLangOpts(LangOpts),
  mBuffer(
    Lexer::getSourceText(CharSourceRange::getTokenRange(SR), SM, LangOpts)),
  mMapping(mBuffer.size() + 1) {
  std::iota(std::begin(mMapping), std::end(mMapping), 0);
}

bool ExternalRewriter::ReplaceText(SourceRange SR, StringRef NewStr) {
  if (mSM.getFileID(SR.getBegin()) != mSM.getFileID(SR.getBegin()))
    return true;
  unsigned Base = mSR.getBegin().getRawEncoding();
  unsigned OrigBegin = SR.getBegin().getRawEncoding() - Base;
  auto ReplacedText =
    Lexer::getSourceText(CharSourceRange::getTokenRange(SR), mSM, mLangOpts);
  unsigned OrigEnd = OrigBegin + ReplacedText.size();
  unsigned Begin = mMapping[OrigBegin];
  unsigned End = mMapping[OrigEnd];
  auto NewStrSize = NewStr.size();
  if (End - Begin < NewStrSize) {
    for (std::size_t I = OrigEnd, EI = mMapping.size(); I < EI; ++I)
      mMapping[I] += NewStrSize - (End - Begin);
  } else if (End - Begin > NewStrSize) {
    for (std::size_t I = OrigEnd, EI = mMapping.size(); I < EI; ++I)
      mMapping[I] -= (End - Begin) - NewStrSize;
  }
  mBuffer.replace(Begin, End - Begin, NewStr);
  return false;
}

StringRef ExternalRewriter::getRewrittenText(clang::SourceRange SR) {
  if (mSM.getFileID(SR.getBegin()) != mSM.getFileID(SR.getBegin()))
    return StringRef();
  unsigned Base = mSR.getBegin().getRawEncoding();
  unsigned OrigBegin = SR.getBegin().getRawEncoding() - Base;
  unsigned Begin = mMapping[OrigBegin];
  auto Text =
    Lexer::getSourceText(CharSourceRange::getTokenRange(SR), mSM, mLangOpts);
  unsigned End = mMapping[OrigBegin + Text.size()];
  return StringRef(mBuffer.data() + Begin, End - Begin);
}
