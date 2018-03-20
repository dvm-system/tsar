#include "tsar_pass.h"
#include <bcl/utility.h>
#include <clang/AST/Stmt.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <vector>
#include <set>

namespace tsar {
struct InterprocElemInfo {
typedef llvm::DenseMap<llvm::Function *,
    std::vector<clang::SourceLocation>> CalleeFuncLoc;
  enum Attr {
    None,
    LibFunc,
    NoReturn,
    EndAttr
  };
  InterprocElemInfo() {}
  void addCalleeFunc(llvm::Function *F,
      std::vector<clang::SourceLocation> VecSL) {
    mCalleeFuncLoc.insert(std::make_pair(F, VecSL));
  }
  CalleeFuncLoc & getCalleeFuncLoc() {
    return mCalleeFuncLoc;
  }
  void setAttr(Attr Type) {
    mAttrs.insert(Type);
  }
  bool hasAttr(Attr Type) {
    return mAttrs.find(Type) != mAttrs.end();
  }
private:
  std::set<Attr> mAttrs;
  CalleeFuncLoc mCalleeFuncLoc;
};
}

namespace tsar {
typedef llvm::DenseMap<llvm::Function *,
    InterprocElemInfo> InterprocAnalysisFuncInfo;
typedef llvm::DenseMap<clang::Stmt *,
    InterprocElemInfo> InterprocAnalysisLoopInfo;
}

namespace llvm {
class InterprocAnalysisPass :
    public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  InterprocAnalysisPass() : ModulePass(ID) {
    initializeInterprocAnalysisPassPass(*PassRegistry::getPassRegistry());
  }
  tsar::InterprocAnalysisFuncInfo & getInterprocAnalysisFuncInfo() noexcept {
    return mInterprocAnalysisFuncInfo;
  }
  tsar::InterprocAnalysisLoopInfo & getInterprocAnalysisLoopInfo() noexcept {
    return mInterprocAnalysisLoopInfo;
  }
  const tsar::InterprocAnalysisFuncInfo &
      getInterprocAnalysisFuncInfo() const noexcept {
    return mInterprocAnalysisFuncInfo;
  }
  const tsar::InterprocAnalysisLoopInfo &
      getInterprocAnalysisLoopInfo() const noexcept {
    return mInterprocAnalysisLoopInfo;
  }
  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
private:
  void runOnSCC(CallGraphSCC &SCC, Module &M);
  tsar::InterprocAnalysisFuncInfo mInterprocAnalysisFuncInfo;
  tsar::InterprocAnalysisLoopInfo mInterprocAnalysisLoopInfo;
};
}
