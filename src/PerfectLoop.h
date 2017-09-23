//=== PerfectLoop.h --- High Level Perfect Loop Analyzer --------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines classes to identify perfect for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CLANG_PERFECT_LOOP_H
#define TSAR_CLANG_PERFECT_LOOP_H

#include "tsar_pass.h"
#include "tsar_utility.h"
#include "DFRegionInfo.h"
#include <utility.h>
#include <llvm/Pass.h>

namespace tsar {

/// Set of perfect loops
typedef std::set<DFNode *> PerfectLoopInfo;
}

namespace llvm {
/// \brief This per-function pass determines perfect for-loops in a source code.
///
/// Before each for-loop perfect/imperfect pragma will be placed. A for-loop is
/// treated as perfect if it has no internal for-loops or if it has only one
/// internal for-loop and there are no other statements between loop bounds.
class ClangPerfectLoopPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  ClangPerfectLoopPass() : FunctionPass(ID) {
    initializeClangPerfectLoopPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns information about loops for an analyzed region.
  tsar::PerfectLoopInfo & getPerfectLoopInfo() noexcept {return mPerfectLoopInfo;}

  /// Returns information about loops for an analyzed region.
  const tsar::PerfectLoopInfo & getPerfectLoopInfo() const noexcept {return mPerfectLoopInfo;}
  
  // Inserts into a source code perfect/imperfect pragma before each for-loop.
  bool runOnFunction(Function &F) override;
  
  void releaseMemory() override {
    mPerfectLoopInfo.clear();
  }

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  tsar::PerfectLoopInfo mPerfectLoopInfo;
};
}
#endif// TSAR_CLANG_PERFECT_LOOP_H
