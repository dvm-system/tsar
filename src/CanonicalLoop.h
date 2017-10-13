//=== CanonicalLoop.h --- High Level Canonical Loop Analyzer ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines classes to identify canonical for-loops in a source code.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CANONICAL_LOOP_H
#define TSAR_CANONICAL_LOOP_H

#include "tsar_pass.h"
#include "tsar_utility.h"
#include <utility.h>
#include <llvm/Pass.h>
#include <set>

namespace tsar {
class DFNode;
/// Set of canonical loops
typedef std::set<DFNode *> CanonicalLoopInfo;
}

namespace llvm {
/// \brief This pass determines canonical for-loops in a source code.
///
/// A for-loop is treated as canonical if it has header like
/// for (/*var initialization*/; /*var comparison*/; /*var increment*/)
class CanonicalLoopPass : public FunctionPass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  CanonicalLoopPass() : FunctionPass(ID) {
    initializeCanonicalLoopPassPass(*PassRegistry::getPassRegistry());
  }

  /// Returns information about loops for an analyzed region.
  tsar::CanonicalLoopInfo & getCanonicalLoopInfo() noexcept {
      return mCanonicalLoopInfo;
  }

  /// Returns information about loops for an analyzed region.
  const tsar::CanonicalLoopInfo & getCanonicalLoopInfo() const noexcept {
      return mCanonicalLoopInfo;
  }
  
  /// Determines canonical loops in a specified functions.
  bool runOnFunction(Function &F) override;
  
  void releaseMemory() override {
    mCanonicalLoopInfo.clear();
  }

  /// Specifies a list of analyzes that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  tsar::CanonicalLoopInfo mCanonicalLoopInfo;
};
}
#endif// TSAR_CANONICAL_LOOP_H