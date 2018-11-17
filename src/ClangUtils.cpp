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
#include "tsar_utility.h"
#include <bcl/utility.h>
#include <clang/Analysis/CFG.h>
#include <clang/ASTMatchers/ASTMatchFinder.h>
#include <clang/Basic/LangOptions.h>
#include <clang/Format/Format.h>
#include <clang/Frontend/ASTUnit.h>
#include <clang/Lex/Lexer.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/raw_ostream.h>
#include <numeric>
#include <regex>

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-utils"

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
  LocalLexer(CharSourceRange::getTokenRange(SR), SM, LangOpts) {}

LocalLexer::LocalLexer(CharSourceRange SR,
    const SourceManager &SM, const LangOptions &LangOpts) :
  mSR(SR), mSM(SM), mLangOpts(LangOpts) {
  assert(mSM.isWrittenInSameFile(mSR.getBegin(), mSR.getEnd()) &&
    "Start and end of the range must be from the same buffer!");
  mCurrentPos = SR.getBegin().getRawEncoding();
  auto SourceText = Lexer::getSourceText(mSR, mSM, mLangOpts);
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

bool tsar::getRawTokenAfter(SourceLocation Loc, const SourceManager &SM,
    const LangOptions &LangOpts, Token &Tok) {
  auto AfterTokenLoc = Lexer::getLocForEndOfToken(Loc, 0, SM, LangOpts);
  if (AfterTokenLoc.isInvalid())
    return true;
  return Lexer::getRawToken(AfterTokenLoc, Tok, SM, LangOpts, true) ||
    Tok.getLocation().isInvalid();
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
  bool IsInvalid = false;
  auto ReplacedText = Lexer::getSourceText(
    CharSourceRange::getTokenRange(SR), mSM, mLangOpts, &IsInvalid);
  if (IsInvalid)
    return true;
  ReplaceText(ComputeOrigOffset(SR.getBegin()), ReplacedText.size(), NewStr);
  return false;
}

void ExternalRewriter::ReplaceText(unsigned OrigBegin, std::size_t Length,
    StringRef NewStr) {
  auto OrigEnd = OrigBegin + Length;
  auto Begin = mMapping[OrigBegin];
  auto End = mMapping[OrigEnd];
  auto NewStrSize = NewStr.size();
  if (End - Begin < NewStrSize) {
    for (std::size_t I = OrigEnd, EI = mMapping.size(); I < EI; ++I)
      mMapping[I] += NewStrSize - (End - Begin);
  } else if (End - Begin > NewStrSize) {
    for (std::size_t I = OrigEnd, EI = mMapping.size(); I < EI; ++I)
      mMapping[I] -= (End - Begin) - NewStrSize;
  }
  mBuffer.replace(Begin, End - Begin, NewStr);
}

bool ExternalRewriter::RemoveText(SourceRange SR, bool RemoveLineIfEmpty) {
  return RemoveText(CharSourceRange::getTokenRange(SR), RemoveLineIfEmpty);
}

bool ExternalRewriter::RemoveText(CharSourceRange SR, bool RemoveLineIfEmpty) {
  if (mSM.getFileID(SR.getBegin()) != mSM.getFileID(SR.getBegin()))
    return true;
  bool IsInvalid;
  auto ReplacedText = Lexer::getSourceText(SR, mSM, mLangOpts, &IsInvalid);
  if (IsInvalid)
    return true;
  unsigned RemoveOffset = ComputeOrigOffset(SR.getBegin());
  std::size_t RemoveLength = ReplacedText.size();
  if (RemoveLineIfEmpty) {
    std::pair<FileID, unsigned> BeginInfo = mSM.getDecomposedLoc(SR.getBegin());
    StringRef Buffer = mSM.getBufferData(BeginInfo.first);
    // Now we check that the line with start of a removed range contains only
    // whitespaces before the start of the range.
    SourceLocation LineStart = getStartOfLine(SR.getBegin(), mSM);
    std::pair<FileID, unsigned> LineInfo = mSM.getDecomposedLoc(LineStart);
    const char *Ptr = Buffer.begin() + LineInfo.second;
    const char *EndPtr = Buffer.begin() + BeginInfo.second;
    bool IsEmpty = true;
    for (; Ptr < EndPtr; ++Ptr) {
      if (!isWhitespace(*Ptr)) {
        IsEmpty = false;
        break;
      }
    }
    // If ok, we should check whether not whitespace characters exist after a
    // specified range.
    if (IsEmpty) {
      // Do not use SR.getEnd() to calculate position after the replaced text,
      // because it may point at the last character in this text or after the
      // last character.
      Ptr = Buffer.begin() + BeginInfo.second + ReplacedText.size();
      EndPtr = Buffer.end();
      std::size_t LineEndOffset = 0;
      for (; Ptr < EndPtr; ++Ptr, ++LineEndOffset) {
        if (isHorizontalWhitespace(*Ptr) || *Ptr == '\r')
          continue;
        if (*Ptr == '\n') {
          RemoveOffset = ComputeOrigOffset(LineStart);
          RemoveLength += BeginInfo.second - LineInfo.second + LineEndOffset + 1;
        }
        break;
      }
    }
  }
  ReplaceText(RemoveOffset, RemoveLength, "");
  return false;
}

StringRef ExternalRewriter::getRewrittenText(clang::SourceRange SR) {
  if (mSM.getFileID(SR.getBegin()) != mSM.getFileID(SR.getBegin()))
    return StringRef();
  unsigned OrigBegin = ComputeOrigOffset(SR.getBegin());
  unsigned Begin = mMapping[OrigBegin];
  auto Text =
    Lexer::getSourceText(CharSourceRange::getTokenRange(SR), mSM, mLangOpts);
  unsigned End = mMapping[OrigBegin + Text.size()];
  return StringRef(mBuffer.data() + Begin, End - Begin);
}

namespace {
/// \brief This matcher searches for a declaration with a specified name and
/// a specified type. It uses a specified function to preprocess a string
/// representation of a declaration type before comparison with a specified
/// type.
///
/// Note, this class does not allocate memory to store strings which are
/// specified in a constructor. So, this reference must be valid while
/// this class is used.
class VarDeclSearch : public ast_matchers::MatchFinder::MatchCallback {
public:
  using ProcessorT =
    std::function<StringRef(StringRef, SmallVectorImpl<char> &)>;

  VarDeclSearch(StringRef Type, StringRef Id, const ProcessorT &P) :
    mType(Type), mId(Id), mProcessor(P) {}

  void run(const ast_matchers::MatchFinder::MatchResult &MR) {
    auto *VD = MR.Nodes.getNodeAs<clang::VarDecl>("varDecl");
    if (!VD)
      return;
    SmallString<32> Buffer;
    mIsFound |= (VD->getName() == mId &&
      mProcessor(VD->getType().getAsString(), Buffer) == mType);
    LLVM_DEBUG({
      bcl::swapMemory(llvm::errs(), llvm::nulls());
      dbgs() << "[BUILD DECLARATION]: candidate type '" << Buffer << "'\n";
      dbgs() << "[BUILD DECLARATION]: candidate id '" << VD->getName() << "'\n";
      if (isFound())
        dbgs() << "[BUILD DECLARATION]: successful build\n";
      bcl::swapMemory(llvm::errs(), llvm::nulls());
    });
  }

  bool isFound() const noexcept { return mIsFound; }

  StringRef getType() const { return mType; }
  StringRef getId() const { return mId; }

private:
  StringRef mType;
  StringRef mId;
  ProcessorT mProcessor;
  bool mIsFound = false;
};
}

std::vector<llvm::StringRef> tsar::buildDeclStringRef(llvm::StringRef Type,
    llvm::StringRef Id, llvm::StringRef Context,
    const llvm::StringMap<std::string> &Replacements) {
  // Custom tokenizer is needed because ASTUnit doesn't have properly
  // setuped Lexer/Rewriter.
  static constexpr const char * Pattern =
    "(struct|union|enum)\\s+|[[:alpha:]_]\\w*|\\d+|\\S";
  auto Tokens = tokenize(Type, Pattern);
  for (auto &T : Tokens) {
    auto Itr = Replacements.find(T);
    if (Itr != Replacements.end())
      T = Itr->getValue();
  }
  // Do not use function parameter 'Type' to initialize search, because
  // some identifiers in 'Type' should be replaced with identifiers from
  // 'Replacements'. So, we explicitly join 'Tokens' to construct new type here.
  SmallString<32> NewType;
  VarDeclSearch Search(join(Tokens.begin(), Tokens.end(), " ", NewType), Id,
    [](StringRef Str, SmallVectorImpl<char> &Out) {
      auto Tokens = tokenize(Str, Pattern);
      return join(Tokens.begin(), Tokens.end(), " ", Out);
  });
  ast_matchers::MatchFinder MatchFinder;
  MatchFinder.addMatcher(ast_matchers::varDecl().bind("varDecl"), &Search);
  Tokens.push_back(Id);
  LLVM_DEBUG(dbgs() << "[BUILD DECLARATION]: context '" << Context << "'\n");
  LLVM_DEBUG(dbgs() << "[BUILD DECLARATION]: type '" << Search.getType() << "'\n");
  LLVM_DEBUG(dbgs() << "[BUILD DECLARATION]: id '" << Search.getId() << "'\n");
  if (Tokens.size() < 2)
    return std::vector<StringRef>();
  // Let us find a valid position for identifier in a variable declaration.
  // Multiple positions can be found in cases like 'unsigned' and 'unsigned int'
  // which mean same type. Since it's part of declaration-specifiers in grammar,
  // it is guaranteed to be before declared identifier, just choose far position
  // (meaning choosing longest type string).
  // Optimization: match in reverse order until success.
  bcl::swapMemory(llvm::errs(), llvm::nulls());
  for (std::size_t Pos = Tokens.size() - 1; ; --Pos) {
    SmallString<128> DeclStr;
    std::unique_ptr<ASTUnit> Unit = tooling::buildASTFromCode(
      Context + join(Tokens.begin(), Tokens.end(), " ", DeclStr) + ";");
    LLVM_DEBUG({
      bcl::swapMemory(llvm::errs(), llvm::nulls());
      SmallString<128> DeclStr;
      dbgs() << "[BUILD DECLARATION]: possible declaration with token at " <<
        Pos << " position '" <<
        join(Tokens.begin(), Tokens.end(), " ", DeclStr) << "'\n";
      bcl::swapMemory(llvm::errs(), llvm::nulls());
    });
    assert(Unit && "AST construction failed");
    // AST can be correctly parsed even with errors.
    // So, we ignore all and just try to find our node.
    MatchFinder.matchAST(Unit->getASTContext());
    if (Search.isFound())
      break;
    if (Pos == 0)
      return std::vector<StringRef>();
    std::swap(Tokens[Pos], Tokens[Pos - 1]);
  }
  bcl::swapMemory(llvm::errs(), llvm::nulls());
  return Tokens;
}

Expected<std::string> tsar::reformat(StringRef TfmSrc, StringRef Filename) {
  using namespace clang::format;
  using namespace clang::tooling;
  std::vector<Range> Ranges({ Range(0, TfmSrc.size()) });
  auto Style = format::getStyle("LLVM", "", "LLVM");
  if (auto Err = Style.takeError())
    return Err;
  // TODO (kaniadnr@gmail.com): it seams that sortInclude() removes adjacent
  // includes with the same name. In the following case:
  //   #include "file.h"
  //   #include "file.h"
  // the result will be
  //    #include "file.h"
  // So, we disable call of sortInclude() at this moment.
  // Replacements Replaces = sortIncludes(*Style, TfmSrc, Ranges, Filename);
  Replacements FormatChanges = reformat(*Style, TfmSrc, Ranges, Filename);
  return applyAllReplacements(TfmSrc, FormatChanges);
}
