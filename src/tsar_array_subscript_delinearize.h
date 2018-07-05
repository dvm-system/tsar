#ifndef TSAR_ARRAY_SUBSCRIPT_DELINEARIZE_H
#define TSAR_ARRAY_SUBSCRIPT_DELINEARIZE_H

#include <llvm/Pass.h>
#include <llvm/IR/Function.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <utility>
#include <set>
#include <map>
#include "tsar_bimap.h"
#include "tsar_pass.h"
#include "tsar_utility.h"
#include <llvm/Analysis/ScalarEvolutionExpressions.h>

#include "tsar_pass.h"
#include "tsar_transformation.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/Pass.h>
#include <llvm/Transforms/Utils/Local.h>
#include <llvm/PassAnalysisSupport.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/ADT/SmallSet.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/ADT/Sequence.h>
#include <llvm/IR/Type.h>


#include <vector>
#include <utility>

namespace llvm {
class Instruction;
class Function;
class Value;
class SCEV;
class ScalarEvolution;
class SCEVMulExpr;
}

namespace tsar {
class Subscript {
public:

  Subscript(const llvm::SCEV *Expr) :
    mExpr(Expr),
    mIsCoefficientsCounted(false) {}

  std::pair<const llvm::SCEV *, const llvm::SCEV *> get—oefficients(llvm::ScalarEvolution &SE);

  bool isConst(llvm::ScalarEvolution &SE);

  const llvm::SCEV* getSCEV() {
    return mExpr;
  }

  void setSCEV(const llvm::SCEV *Expr) {
    mExpr = Expr;
    mIsCoefficientsCounted = false;
  }

private:
  std::pair<const llvm::SCEV *, const llvm::SCEV *> findCoefficientsInSCEVMulExpr(const llvm::SCEVMulExpr *MulExpr,
    llvm::ScalarEvolution &SE);

  std::pair<const llvm::SCEV *, const llvm::SCEV *> findCoefficientsInSCEV(const llvm::SCEV *Expr,
    llvm::ScalarEvolution &SE);

  const llvm::SCEV *mExpr;
  std::pair<const llvm::SCEV *, const llvm::SCEV *> m—oefficients;
  bool mIsCoefficientsCounted;
};

struct ArrayAccess {
  llvm::SmallVector<Subscript, 3> mSubscripts;
  llvm::Instruction *mAccessInstruction;
};

struct Array {
  //EstimateMemory *mArray;
  Array(llvm::Value *Root) :
    mRoot(Root) {}

  llvm::Value *mRoot;
  llvm::SmallVector<const llvm::SCEV *, 3> mDims;
  llvm::SmallVector<ArrayAccess, 3> mAccesses;
};
}

namespace llvm {
class ArraySubscriptDelinearizePass :
  public FunctionPass, private bcl::Uncopyable {
public:

  typedef std::map <Instruction *, SmallVector<
    std::pair<const SCEV *, const SCEV *>, 3>> ArraySubscriptDelinearizeInfo;

  typedef std::set<Instruction *> ArraySubscriptSet;

  static char ID;

  ArraySubscriptDelinearizePass() : llvm::FunctionPass(ID) {
    initializeArraySubscriptDelinearizePassPass(*llvm::PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override;

  const SmallVectorImpl<tsar::Array> & getAnalyzedArrays() const noexcept { return mAnalyzedArrays; }

  const ArraySubscriptDelinearizeInfo & getDelinearizedSubscripts() const noexcept { return mDelinearizedSubscripts; }

private:
  ArraySubscriptDelinearizeInfo mDelinearizedSubscripts;
  SmallVector<tsar::Array, 3> mAnalyzedArrays;
};
}

#endif //TSAR_ARRAY_SUBSCRIPT_DELINEARIZE_H