//=== CanonicalLoop.h --- High Level Canonical Loop Analyzer ----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file defines classes to identify canonical for-loops in a source code.
// Canonical Loop Form is described here:
// http://www.openmp.org/wp-content/uploads/openmp-4.5.pdf#page=62
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_CANONICAL_LOOP_H
#define TSAR_CANONICAL_LOOP_H

#include "tsar_pass.h"
#include "tsar_utility.h"
#include <utility.h>
#include <llvm/Pass.h>
#include <set>

namespace clang {
class Stmt;
}

namespace llvm {
class Instruction;
}

namespace tsar {
class DFNode;
typedef bcl::tagged_pair<
    bcl::tagged<clang::Stmt *, AST>,
    bcl::tagged<llvm::Instruction *, IR>> NodeInstruction;
class LoopInfo {
public:
  LoopInfo(DFNode *Node) : mIsCanonical(false), mNode(Node),
      Initialization(NodeInstruction(nullptr, nullptr)),
      Increment(NodeInstruction(nullptr, nullptr)),
      Condition(NodeInstruction(nullptr, nullptr)) {}
    
  void setCanonical() { mIsCanonical = true; }
  void setStmts(const clang::Stmt *Init, const clang::Stmt *Incr,
      const clang::Stmt *Cond) {
    Initialization = NodeInstruction(const_cast<clang::Stmt*>(Init),
        Initialization.second);
    Increment = NodeInstruction(const_cast<clang::Stmt*>(Incr),
        Increment.second);
    Condition = NodeInstruction(const_cast<clang::Stmt*>(Cond),
        Condition.second);
  }
  void setInstructions(llvm::Instruction *Init, llvm::Instruction *Incr,
      llvm::Instruction *Cond) {
    Initialization = NodeInstruction(Initialization.first, Init);
    Increment = NodeInstruction(Increment.first, Incr);
    Condition = NodeInstruction(Condition.first, Cond);
  };

  bool isCanonical() { return mIsCanonical; }
  const bool isCanonical() const { return mIsCanonical; }
  DFNode * getNode() { return mNode; }
  const DFNode * getNode() const { return mNode; }
  NodeInstruction * getInit() { return &Initialization; }
  const NodeInstruction * getInit() const { return &Initialization; }
  NodeInstruction * getInc() { return &Increment; }
  const NodeInstruction * getInc() const { return &Increment; }
  NodeInstruction * getCond() { return &Condition; }
  const NodeInstruction * getCond() const { return &Condition; }
    
private:
  bool mIsCanonical;
  DFNode *mNode;
  NodeInstruction Initialization, Increment, Condition;
};
struct LoopMapInfo {
  static inline LoopInfo * getEmptyKey() {
    return llvm::DenseMapInfo<LoopInfo *>::getEmptyKey();
  }
  static inline LoopInfo * getTombstoneKey() {
    return llvm::DenseMapInfo<LoopInfo *>::getTombstoneKey();
  }
  static inline unsigned getHashValue(const LoopInfo *Info) {
    return llvm::DenseMapInfo<DFNode *>::getHashValue(Info->getNode());
  }
  static inline unsigned getHashValue(const DFNode *N) {
    return llvm::DenseMapInfo<DFNode *>::getHashValue(N);
  }
  static inline bool isEqual(const LoopInfo *LHS, const LoopInfo *RHS) {
    return LHS == RHS;
  }
  static inline bool isEqual(const DFNode *LHS, const LoopInfo *RHS) {
    return !isEqual(RHS, getEmptyKey()) && ! isEqual(RHS, getTombstoneKey())
        && LHS == RHS->getNode();
  }
};
typedef llvm::DenseSet<const LoopInfo *, LoopMapInfo> CanonicalLoopInfo;
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