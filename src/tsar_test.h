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

namespace tsar {
/// Inserts result of analysis into a source file.
class TestQueryManager : public QueryManager {
  void run(llvm::Module *M, TransformationContext *Ctx) override;
};
}

namespace llvm {
/// Inserts result of analysis into a source file.
class TestPrinterPass :
public ModulePass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// This is a SAPFOR Analysis pragma.
  static constexpr char * mAnalysisPragma = "#pragma sapfor analysis";

  /// Default constructor.
  TestPrinterPass() : ModulePass(ID) {
    initializeTestPrinterPassPass(*PassRegistry::getPassRegistry());
  }

  /// Inserts result of analysis into the source file.
  bool runOnModule(llvm::Module &M) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};
}
#endif//TSAR_TEST_H

