//===--- tsar_test.cpp ------ Test Result Printer ---------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements pass to print test results.
//
//===----------------------------------------------------------------------===//

#include <clang/AST/Decl.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/Stmt.h>
#include <llvm/Analysis/Passes.h>
#if (LLVM_VERSION_MAJOR > 2 && LLVM_VERSION_MINOR > 7 || LLVM_VERSION_MAJOR > 3)
#include <llvm/Analysis/BasicAliasAnalysis.h>
#endif
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <llvm/Transforms/IPO/FunctionAttrs.h>
#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <vector>
#include "CanonicalLoop.h"
#include "DefinedMemory.h"
#include "DIUnparser.h"
#include "EstimateMemory.h"
#include "tsar_fcopy_elimination.h"
#include "tsar_loop_matcher.h"
#include "tsar_memory_matcher.h"
#include "PerfectLoop.h"
#include "tsar_private.h"
#include "tsar_pass_provider.h"
#include "tsar_test.h"
#include "tsar_trait.h"
#include "tsar_transformation.h"
#include "tsar_utility.h"

using namespace tsar;
using namespace llvm;
using namespace clang;
using ::llvm::Module;

#undef DEBUG_TYPE
#define DEBUG_TYPE "test-printer"

#if (LLVM_VERSION_MAJOR < 4 && LLVM_VERSION_MINOR < 8)
namespace {
class TestPrinterProvider : public FunctionPassProvider<
  PrivateRecognitionPass, TransformationEnginePass,
  LoopMatcherPass, DFRegionInfoPass, ClangPerfectLoopPass,
  CanonicalLoopPass, MemoryMatcherImmutableWrapper> {
  void getAnalysisUsage(llvm::AnalysisUsage &AU) const override {
    auto P = createBasicAliasAnalysisPass();
    AU.addRequiredID(P->getPassID());
    delete P;
    FunctionPassProvider::getAnalysisUsage(AU);
  }
};
}
#else
typedef FunctionPassProvider<
  DominatorTreeWrapperPass,
  BasicAAWrapperPass,
  PrivateRecognitionPass,
  TransformationEnginePass,
  LoopMatcherPass,
  DFRegionInfoPass,
  ClangPerfectLoopPass,
  CanonicalLoopPass,
  CopyEliminationPass,
  MemoryMatcherImmutableWrapper> TestPrinterProvider;
#endif

INITIALIZE_PROVIDER_BEGIN(TestPrinterProvider, "test-provider",
  "Test Printer Provider")
INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PASS_DEPENDENCY(DFRegionInfoPass)
INITIALIZE_PASS_DEPENDENCY(ClangPerfectLoopPass)
INITIALIZE_PASS_DEPENDENCY(CanonicalLoopPass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_DEPENDENCY(CopyEliminationPass)
INITIALIZE_PROVIDER_END(TestPrinterProvider, "test-provider",
  "Test Printer Provider")

char TestPrinterPass::ID = 0;
INITIALIZE_PASS_BEGIN(TestPrinterPass, "test-printer",
  "Test Result Printer", true, true)
INITIALIZE_PASS_DEPENDENCY(TestPrinterProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(MemoryMatcherImmutableWrapper)
INITIALIZE_PASS_END(TestPrinterPass, "test-printer",
  "Test Result Printer", true, true)

namespace {
/// Prints an appropriate clause for each trait to a specified output stream.
class TraitClausePrinter {
public:
  /// Creates functor.
  explicit TraitClausePrinter(const AliasTree *AT, const DominatorTree &DT,
      llvm::raw_ostream &OS) : mAliasTree(AT), mDT(&DT), mOS(OS) {}

  /// \brief Prints an appropriate clause for each trait in the vector.
  ///
  /// Variable names in analysis clauses is printed in alphabetical order and do
  /// not change from run to run.
  template<class Trait> void operator()(
      std::vector<AliasTrait *> &TraitVector) {
    if (TraitVector.empty())
      return;
    std::string Clause = Trait::toString();
    Clause.erase(std::remove(Clause.begin(), Clause.end(), ' '), Clause.end());
    std::set<std::string, std::less<std::string>> SortedVarList;
    for (auto *AT : TraitVector) {
      if (AT->getNode() == mAliasTree->getTopLevelNode())
        continue;
      for (auto &T : *AT) {
        if (!std::is_same<Trait, trait::AddressAccess>::value &&
              T.is<trait::NoAccess>() ||
            std::is_same<Trait, trait::AddressAccess>::value && !T.is<Trait>())
          continue;
        std::string Str;
        raw_string_ostream TmpOS(Str);
        TmpOS << '<';
        if (!unparsePrint(TmpOS, T.getMemory()->front(), mDT))
          TmpOS << '?';
        TmpOS << ',';
        if (T.getMemory()->isSized())
          TmpOS << T.getMemory()->getSize();
        else
          TmpOS << '?';
        TmpOS << '>';
        SortedVarList.insert(TmpOS.str());
      }
    }
    if (SortedVarList.empty())
      return;
    mOS << " " << Clause << "(";
    auto I = SortedVarList.begin(), EI = SortedVarList.end();
    for (--EI; I != EI; ++I)
      mOS << *I << ", ";
    mOS << *I;
    mOS << ")";
  }

private:
  const AliasTree *mAliasTree;
  const DominatorTree *mDT;
  raw_ostream &mOS;
};

/// Returns a filename adjuster which adds .test after the file name.
inline FilenameAdjuster getTestFilenameAdjuster() {
  return [](llvm::StringRef Filename) -> std::string {
    SmallString<128> Path = Filename;
    sys::path::replace_extension(Path, ".test" + sys::path::extension(Path));
    return Path.str();
  };
}
}

bool TestPrinterPass::runOnModule(llvm::Module &M) {
  auto TfmCtx = getAnalysis<TransformationEnginePass>().getContext(M);
  if (!TfmCtx || !TfmCtx->hasInstance()) {
    errs() << "error: can not transform sources for the module "
      << M.getName() << "\n";
    return false;
  }
  TestPrinterProvider::initialize<TransformationEnginePass>(
    [&M, &TfmCtx](TransformationEnginePass &TEP) {
      TEP.setContext(M, TfmCtx);
  });
  auto &MMWrapper = getAnalysis<MemoryMatcherImmutableWrapper>();
  TestPrinterProvider::initialize<MemoryMatcherImmutableWrapper>(
    [&MMWrapper](MemoryMatcherImmutableWrapper &Wrapper) {
      Wrapper.set(*MMWrapper);
  });
  auto &Rewriter = TfmCtx->getRewriter();
  for (Function &F : M) {
    if (F.empty())
      continue;
    auto &Provider = getAnalysis<TestPrinterProvider>(F);
    auto &DT = Provider.get<DominatorTreeWrapperPass>().getDomTree();
    auto &LMP = Provider.get<LoopMatcherPass>();
    auto &LpMatcher = LMP.getMatcher();
    auto FuncDecl = LMP.getFunctionDecl();
    auto &PrivateInfo = Provider.get<PrivateRecognitionPass>().getPrivateInfo();
    auto &RegionInfo = Provider.get<DFRegionInfoPass>().getRegionInfo();
    auto &PLoopInfo = Provider.get<ClangPerfectLoopPass>().getPerfectLoopInfo();
    auto &CLoopInfo = Provider.get<CanonicalLoopPass>().getCanonicalLoopInfo();
    for (auto &Match : LpMatcher) {
      if (!isa<ForStmt>(Match.get<AST>()) &&
          !isa<WhileStmt>(Match.get<AST>()) &&
          !isa<DoStmt>(Match.get<AST>()))
        printPragma(Match.get<AST>()->getLocStart(), Rewriter,
          [](raw_ostream &OS) {OS << " loop(" << mImplicitLoopClause << ")"; });
      auto N = RegionInfo.getRegionFor(Match.get<IR>());
      auto DSItr = PrivateInfo.find(N);
      assert(DSItr != PrivateInfo.end() && DSItr->get<DependencySet>() &&
        "Privatiability information must be specified!");
      typedef bcl::StaticTraitMap<
        std::vector<AliasTrait *>, MemoryDescriptor> TraitMap;
      TraitMap TM;
      for (auto &TS : *DSItr->get<DependencySet>())
        TS.for_each(
          bcl::TraitMapConstructor<AliasTrait, TraitMap>(TS, TM));
      auto *AT = DSItr->get<DependencySet>()->getAliasTree();
      printPragma(Match.get<AST>()->getLocStart(), Rewriter,
        [&TM, AT, &DT](raw_ostream &OS) {
          TM.for_each(TraitClausePrinter(AT, DT, OS));
      });
      if (PLoopInfo.find(N) != PLoopInfo.end()) {
        printPragma(Match.get<AST>()->getLocStart(), Rewriter,
          [](raw_ostream &OS) { OS << " " << mPerfectLoopClause; });
      } else {
        printPragma(Match.get<AST>()->getLocStart(), Rewriter,
          [](raw_ostream &OS) { OS << " " << mImperfectLoopClause; });
      }
      auto I = CLoopInfo.find_as(N);
      if ((I != CLoopInfo.end()) && ((**I).isCanonical())) {
        printPragma(Match.get<AST>()->getLocStart(), Rewriter,
          [](raw_ostream &OS) { OS << " " << mCanonicalLoopClause; });
      } else {
        printPragma(Match.get<AST>()->getLocStart(), Rewriter,
          [](raw_ostream &OS) { OS << " " << mNonCanonicalLoopClause; });
      }
    }
    for (auto L : LMP.getUnmatchedAST()) {
      printPragma(L->getLocStart(), Rewriter,
        [](raw_ostream &OS) { OS << " " << mUnavailableClause; });
    }
  }
  TfmCtx->release(getTestFilenameAdjuster());
  return false;
}

template<class Function>
void TestPrinterPass::printPragma(
    const SourceLocation &StartLoc, Rewriter &R, Function &&F) const {
  std::string PragmaStr;
  raw_string_ostream OS(PragmaStr);
  auto &SrcMgr = R.getSourceMgr();
  assert(!StartLoc.isInvalid() && "Invalid location!");
  // If loop is in a macro the '\' should be added before the line end.
  const char * EndLine = StartLoc.isMacroID() ? " \\\n" : "\n";
  auto SpellLoc = SrcMgr.getSpellingLoc(StartLoc);
  if (!isLineBegin(SrcMgr, SpellLoc))
    OS << EndLine;
  OS << mAnalysisPragma;
  F(OS);
  printExpansionClause(SrcMgr, StartLoc, OS);
  OS << EndLine;
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
  R.InsertText(SpellLoc, OS.str(), true, true);
}

void TestPrinterPass::printExpansionClause(
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

bool TestPrinterPass::isLineBegin(
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
  return Column >= SrcMgr.getColumnNumber(FID, StartOffs);
}

void TestPrinterPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TestPrinterProvider>();
  AU.addRequired<TransformationEnginePass>();
  AU.addRequired<MemoryMatcherImmutableWrapper>();
  AU.setPreservesAll();
}

ModulePass * llvm::createTestPrinterPass() {
  return new TestPrinterPass();
}

void TestQueryManager::run(llvm::Module *M, TransformationContext *Ctx) {
  assert(M && "Module must not be null!");
  legacy::PassManager Passes;
  if (Ctx) {
    auto TEP = static_cast<TransformationEnginePass *>(
      createTransformationEnginePass());
    TEP->setContext(*M, Ctx);
    Passes.add(TEP);
  }
  Passes.add(createUnreachableBlockEliminationPass());
  Passes.add(createPostOrderFunctionAttrsLegacyPass());
  Passes.add(createMemoryMatcherPass());
  Passes.add(createTestPrinterPass());
  Passes.add(createVerifierPass());
  Passes.run(*M);
}
