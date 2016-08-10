//===--- tsar_test.h ----------- Test Result Printer ------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// Each tool must have a set of tests to check its functionality. This file
// defines a pass that prints results of analysis in a test form so this results
// can be checked manually or with with specially developed separated tools.
// This pass insert special directives #pragma tsar ... (for C language) into an
// analyzed sources before each element that has been analyzed.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_TEST_H
#define TSAR_TEST_H

#include <llvm/Pass.h>
#include <utility.h>
#include "tsar_query.h"
#include "tsar_pass.h"

namespace clang {
class SourceLocation;
class SourceManager;
class Rewriter;
}

namespace tsar {
/// Inserts result of analysis into a source file.
class TestQueryManager : public QueryManager {
  void run(llvm::Module *M, TransformationContext *Ctx) override;
};
}

namespace llvm {
class raw_ostream;

/// Inserts result of analysis into a source file.
class TestPrinterPass :
public ModulePass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// This is a SAPFOR Analysis pragma.
  static constexpr char * mAnalysisPragma = "#pragma sapfor analysis";

  /// This is a SAPFOR Unavailable clause.
  static constexpr char * mUnavailableClause = "unavailable";

  /// This is a SAPFOR Implicit Loop clause.
  static constexpr char *mImplicitLoopClause = "implicit";

  /// This is a SAPFOR Expansion clause.
  static constexpr char *mExpansionClause = "expansion";

  /// This is a SAPFOR Include clause.
  static constexpr char *mIncludeClause = "include";

  /// Default constructor.
  TestPrinterPass() : ModulePass(ID) {
    initializeTestPrinterPassPass(*PassRegistry::getPassRegistry());
  }

  /// Inserts result of analysis into the source file.
  bool runOnModule(llvm::Module &M) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  /// Return true if a specified location is at the beginning of its line
  /// (may be preceded by whitespaces)
  bool isLineBegin(clang::SourceManager &SrcMgr,
    clang::SourceLocation &Loc) const;

  /// \brief Prints expansion and include clauses if a specified location is an
  /// expansion location or is presented in a file that has been included.
  ///
  /// Expansion clause: (" expansion(filename:line:column)")
  /// Include clause: (" include(filename:line:column)")
  /// TODO (kaniandr@gmail.com):
  /// 1. Line and column in expansion clause should be calculated relative to
  /// transformed file. At this moment they calculated relative to the original
  /// file.
  /// 2. If some macro expanded multiple times empty line will be inserted
  /// between different pragmas:
  /// \code
  /// #pragma sapfor analysis ... expansion(...) \
  /// \
  /// #pragma sapfor analysis ... expansion(...) \
  /// ...
  /// \endcode
  /// This empty line should be removed.
  void printExpansionClause(clang::SourceManager &SrcMgr,
    const clang::SourceLocation &Loc, llvm::raw_ostream &OS) const;

  /// \brief Prints SAPFOR Analysis pragma.
  ///
  /// A pragma is going to start at spelling location computed for StartLoc.
  /// The pragma will have a following structur:
  /// `#pragma sapfor analysis <user-clauses> [expansion(...)]`. Expansion
  /// clause is printed for a location in macro. User clauses determined by a
  /// function F, which should accept single argument `llvm::raw_ostream &`.
  template<class Function>
  void printPragma(const clang::SourceLocation &StartLoc, clang::Rewriter &R,
    Function &&F) const;
};
}
#endif//TSAR_TEST_H

