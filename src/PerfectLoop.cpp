//=== PerfectLoop.cpp - High Level Perfect Loop Analyzer --------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements classes to identify perfect for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#include "PerfectLoop.h"
#include "tsar_transformation.h"
#include <clang/AST/Decl.h>
#include <clang/AST/RecursiveASTVisitor.h>
#include <clang/AST/Stmt.h>
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/IR/Module.h>
#include <typeinfo>

using namespace llvm;
using namespace clang;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "perfect-loop"

STATISTIC(NumPerfect, "Number of perfectly nested for-loops");
STATISTIC(NumImPerfect, "Number of imperfectly nested for-loops");

char ClangPerfectLoopPass::ID = 0;
INITIALIZE_PASS_BEGIN(ClangPerfectLoopPass, "perfect-loop",
  "Perfectly Nested Loop Analysis", true, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(ClangPerfectLoopPass, "perfect-loop",
  "Perfectly Nested Loop Analysis", true, true)

namespace {
/// This visits and analyzes all for-loops in a source code.
class LoopVisitor : public RecursiveASTVisitor<LoopVisitor> {
public:
  /// Creates visitor.
  explicit LoopVisitor(Rewriter &R) : mRewriter(&R) {}

  /// Inserts appropriate pragma before a for-loop.
  /// Calls TraverseStmt() that walks through the body of cycle
  bool TraverseForStmt(ForStmt *For) {
    // Keeping values of external cycle if it exists
    int PrevNumberOfLoops = ++mNumberOfLoops;
    bool PrevIsThereOperators = mIsThereOperators;
    // Starting analysis
    mNumberOfLoops = 0;
    mIsThereOperators = false;
    // Preparing output
    std::string PragmaStr;
    raw_string_ostream OS(PragmaStr);
    auto StartLoc = For->getLocStart();
    assert(!StartLoc.isInvalid() && "Invalid location!");
    // If loop is in a macro the '\' should be added before the line end.
    const char * EndLine = StartLoc.isMacroID() ? " \\\n" : "\n";
    auto &SrcMgr = mRewriter->getSourceMgr();
    auto SpellLoc = SrcMgr.getSpellingLoc(StartLoc);
    // The loop should be moved to a new line if it is located in the middle of
    // the line.
    if (!isLineBegin(SrcMgr, SpellLoc))
      OS << EndLine;
    OS << mAnalysisPragmaStart << " ";
    // Here goes traverse
    auto Res = RecursiveASTVisitor::TraverseStmt(For->getBody());
    // Analyzing data
    if (((mNumberOfLoops == 1) && (!mIsThereOperators)) || (mNumberOfLoops == 0)) {
      OS << mPerfectLoopClause;
      ++NumPerfect;
    } else {
      OS << mImperfectLoopClause;
      ++NumImPerfect;
    }
    printExpansionClause(SrcMgr, StartLoc, OS);
    OS << mAnalysisPragmaEnd << EndLine;
    // Analysis ended
    // Return values of external cycle
    mNumberOfLoops = PrevNumberOfLoops;
    mIsThereOperators = PrevIsThereOperators;
    // If one file has been included multiple times there are different FileID
    // for each include. So to combine transformation of each include in a single
    // file we recalculate the SpellLoc location.
    static StringMap<FileID> FileNameToId;
    auto DecLoc = SrcMgr.getDecomposedLoc(SpellLoc);
    auto Pair = FileNameToId.insert(
      std::make_pair(SrcMgr.getFilename(SpellLoc), DecLoc.first));
    if (!Pair.second && Pair.first != FileNameToId.end()) {
      // File with such name has been already transformed.
      auto FileStartLoc = SrcMgr.getLocForStartOfFile(Pair.first->second);
      SpellLoc = FileStartLoc.getLocWithOffset(DecLoc.second);
    }
    mRewriter->InsertText(SpellLoc, OS.str(), true, true);
    return true;
  }

  /// Actually visiting statements
  /// Called from TraverseStmt(), replaces VisitStmt()
  bool VisitStmt(Stmt *Statement) {
      if (!llvm::isa<CompoundStmt>(Statement))
        mIsThereOperators = true;
      return true;
  }

private:
  /// Return true if a specified location is at the beginning of its line
  /// (may be preceded by whitespaces)
  bool isLineBegin(
    SourceManager &SrcMgr, SourceLocation &Loc) const {
    FileID FID;
    unsigned StartOffs;
    std::tie(FID, StartOffs) = SrcMgr.getDecomposedLoc(Loc);
    StringRef MB = SrcMgr.getBufferData(FID);
    unsigned LineNo = SrcMgr.getLineNumber(FID, StartOffs) - 1;
    const SrcMgr::ContentCache *Content =
      SrcMgr.getSLocEntry(FID).getFile().getContentCache();
    unsigned LineOffs = Content->SourceLineCache[LineNo];
    unsigned Column;
    for (Column = LineOffs; bcl::isWhitespace(MB[Column]); ++Column);
    Column -= LineOffs; ++Column; // The first column without a whitespace.
    if (Column < SrcMgr.getColumnNumber(FID, StartOffs))
      return false;
    return true;
  }

  /// \brief Prints expansion and include clauses if a specified location is an
  /// expansion location or is presented in a file that has been included.
  ///
  /// Expansion clause: (" expansion(filename:line:column)")
  /// Include clause: (" include(filename:line:column)")
  void printExpansionClause(
    SourceManager &SrcMgr, const SourceLocation &Loc, raw_ostream &OS) const {
    if (!Loc.isValid())
      return;
    if (Loc.isMacroID()) {
      auto PLoc = SrcMgr.getPresumedLoc(Loc);
      OS << " " << mExpansionClause << "("
        << sys::path::filename(PLoc.getFilename()) << ":"
        << PLoc.getLine() << ":" << PLoc.getColumn()
        << ")";
    }
    auto IncludeLoc = SrcMgr.getIncludeLoc(SrcMgr.getFileID(Loc));
    if (IncludeLoc.isValid()) {
      auto PLoc = SrcMgr.getPresumedLoc(IncludeLoc);
      OS << " " << mIncludeClause << "("
        << sys::path::filename(PLoc.getFilename()) << ":"
        << PLoc.getLine() << ":" << PLoc.getColumn()
        << ")";
    }
  }

  /// This is a SAPFOR Analysis pragma.
  static constexpr const char * mAnalysisPragmaStart =
    "_Pragma(\"sapfor analysis";

  /// This is a SAPFOR Analysis pragma.
  static constexpr const char * mAnalysisPragmaEnd = "\")";

  /// This is a SAPFOR Expansion clause.
  static constexpr const char *mExpansionClause = "expansion";

  /// This is a SAPFOR Include clause.
  static constexpr const char *mIncludeClause = "include";

  /// This is a SAPFOR Perfect Loop clause.
  static constexpr const char *mPerfectLoopClause = "perfect";

  /// This is a SAPFOR Imperfect Loop clause.
  static constexpr const char *mImperfectLoopClause = "imperfect";
  
  // This is number of loops,existence of none-cycle operators
  // inside the analyzed one's body
  int mNumberOfLoops;
  bool mIsThereOperators;

  Rewriter *mRewriter;
};

/// Returns a filename adjuster which adds .test after the file name.
inline FilenameAdjuster getPerfectLoopFilenameAdjuster() {
  return [](llvm::StringRef Filename) -> std::string {
    SmallString<128> Path = Filename;
    sys::path::replace_extension(Path, ".perfect" + sys::path::extension(Path));
    return Path.str();
  };
}
}

bool ClangPerfectLoopPass::runOnFunction(Function &F) {
  auto M = F.getParent();
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl = TfmCtx->getDeclForMangledName(F.getName());
  if (!FuncDecl)
    return false;
  LoopVisitor Visitor(TfmCtx->getRewriter());
  Visitor.TraverseDecl(FuncDecl);
  TfmCtx->release(getPerfectLoopFilenameAdjuster());
  return false;
}

void ClangPerfectLoopPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

FunctionPass *llvm::createClangPerfectLoopPass() {
  return new ClangPerfectLoopPass();
}
