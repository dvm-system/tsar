#include "tsar_pass.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/Function.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <clang/Basic/SourceLocation.h>
#include <vector>
#include <set>

namespace {
const std::set<llvm::Attribute::AttrKind> AndAttrs = {
    llvm::Attribute::ReadOnly,
    llvm::Attribute::NoUnwind
};
const std::set<llvm::Attribute::AttrKind> OrAttrs = {
  llvm::Attribute::NoReturn
};

struct InterprocFuncInfo {
  InterprocFuncInfo() : isLibFunc(false) {}
  void addCalleeFunc(
      llvm::Function *F, std::vector<clang::SourceLocation> VecSL) {
    mVecCalleeFunc.push_back(std::make_pair(F, VecSL));
  }
  void setLibFunc(bool Val) {
    isLibFunc = Val;
  }
  bool getLibFunc() {
    return isLibFunc;
  }
private:
  bool isLibFunc;
  std::vector<std::pair<llvm::Function *,
      std::vector<clang::SourceLocation>>> mVecCalleeFunc;
};
}

namespace tsar {
typedef llvm::DenseMap<llvm::Function *,
    InterprocFuncInfo> InterprocAnalysisInfo;
}

namespace llvm {
class InterprocAnalysisPass :
    public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  InterprocAnalysisPass() : ModulePass(ID) {
    initializeInterprocAnalysisPassPass(*PassRegistry::getPassRegistry());
  }
  tsar::InterprocAnalysisInfo & getInterprocAnalysisInfo() noexcept {
    return mInterprocAnalysisInfo;
  }
  const tsar::InterprocAnalysisInfo &
      getInterprocAnalysisInfo() const noexcept {
    return mInterprocAnalysisInfo;
  }
  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
private:
  void runOnSCC(CallGraphSCC &SCC, Module &M);
  tsar::InterprocAnalysisInfo mInterprocAnalysisInfo;
};
}
