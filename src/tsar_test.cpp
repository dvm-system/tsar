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
#include "clang/AST/PrettyPrinter.h"
#include <clang/AST/Stmt.h>
#include <llvm/Analysis/Passes.h>
#include <llvm/CodeGen/Passes.h>
#include <llvm/IR/LegacyPassManagers.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <algorithm>
#include <limits>
#include <string>
#include <vector>
#include "tsar_df_location.h"
#include "tsar_loop_matcher.h"
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
  PrivateRecognitionPass, TransformationEnginePass, LoopMatcherPass> {
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
  BasicAAWrapperPass,
  PrivateRecognitionPass,
  TransformationEnginePass,
  LoopMatcherPass> TestPrinterProvider;
#endif

INITIALIZE_PROVIDER_BEGIN(TestPrinterProvider, "test-provider",
  "Test Printer Provider")
INITIALIZE_PASS_DEPENDENCY(PrivateRecognitionPass)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_DEPENDENCY(LoopMatcherPass)
INITIALIZE_PROVIDER_END(TestPrinterProvider, "test-provider",
  "Test Printer Provider")

char TestPrinterPass::ID = 0;
INITIALIZE_PASS_BEGIN(TestPrinterPass, "test-printer",
  "Test Result Printer", true, true)
INITIALIZE_PASS_DEPENDENCY(TestPrinterProvider)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(TestPrinterPass, "test-printer",
  "Test Result Printer", true, true)

namespace {
/// Prints an appropriate clause for each trait to a specified output stream.
class TraitClausePrinter {
public:
  /// Creates functor.
  explicit TraitClausePrinter(llvm::raw_ostream &OS) : mOS(OS) {}

  /// Prints an appropriate clause for each trait in the vector.
  template<class Trait> void operator()(
      std::vector<LocationTraitSet *> &TraitVector) {
    if (TraitVector.empty())
      return;
    std::string Clause = Trait::toString();
    Clause.erase(std::remove(Clause.begin(), Clause.end(), ' '), Clause.end());
    mOS << " " << Clause << "(";
    auto I = TraitVector.begin();
    for (auto EI = TraitVector.end() - 1; I != EI; ++I) {
      mOS << locationToSource((*I)->memory()->Ptr) << ", ";
    }
    mOS << locationToSource((*I)->memory()->Ptr);
    mOS << ")";
  }

private:
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
  auto &Rewriter = TfmCtx->getRewriter();
  for (Function &F : M) {
    if (F.empty())
      continue;
    auto &Provider = getAnalysis<TestPrinterProvider>(F);
    auto &LMP = Provider.get<LoopMatcherPass>();
    auto &LpMatcher = LMP.getMatcher();
    auto FuncDecl = LMP.getFunctionDecl();
    auto &PRP = Provider.get<PrivateRecognitionPass>();
    for (auto &Match : LpMatcher) {
      auto &DS = PRP.getPrivatesFor(Match.get<IR>());
      typedef bcl::StaticTraitMap<
        std::vector<LocationTraitSet *>, DependencyDescriptor> TraitMap;
      TraitMap TM;
      for (auto &TS : DS)
        TS.for_each(
          bcl::TraitMapConstructor<LocationTraitSet, TraitMap>(TS, TM));
      std::string PragmaStr;
      raw_string_ostream OS(PragmaStr);
      TM.for_each(TraitClausePrinter(OS << mAnalysisPragma));
      OS << "\n";
      Rewriter.InsertText(Match.get<AST>()->getForLoc(), OS.str(), true, true);
    }
  }
  TfmCtx->release(getTestFilenameAdjuster());
  return false;
}

void TestPrinterPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TestPrinterProvider>();
  AU.addRequired<TransformationEnginePass>();
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
  Passes.add(createTestPrinterPass());
  Passes.add(createVerifierPass());
  cl::PrintOptionValues();
  Passes.run(*M);
}
