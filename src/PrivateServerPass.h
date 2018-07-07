//===- PrivateServerPass.h ---- Test Result Printer -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines a pass to interact with client software and to provide
// results of loop traits analysis. Initially the pass provides statistics of
// analysis results.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PRIVATE_SERVER_H
#define TSAR_PRIVATE_SERVER_H

#include <map>
#include <llvm/Pass.h>
#include <bcl/utility.h>
#include "tsar_pass.h"

namespace bcl {
class IntrusiveConnection;
class RedirectIO;
}

namespace llvm {
class raw_ostream;

/// Interacts with a client and sends result of analysis on request.
class PrivateServerPass :
  public ModulePass, private bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  PrivateServerPass() : ModulePass(ID), mConnection(nullptr) {
    initializePrivateServerPassPass(*PassRegistry::getPassRegistry());
  }

  /// Constructor.
  explicit PrivateServerPass(bcl::IntrusiveConnection &IC,
      bcl::RedirectIO &StdErr) :
    ModulePass(ID), mConnection(&IC), mStdErr(&StdErr) {
    initializePrivateServerPassPass(*PassRegistry::getPassRegistry());
  }

  /// Interacts with a client and sends result of analysis on request.
  bool runOnModule(llvm::Module &M) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

private:
  bcl::IntrusiveConnection *mConnection;
  bcl::RedirectIO *mStdErr;
};
}
#endif//TSAR_PRIVATE_SERVER_H


