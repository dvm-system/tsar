#include "tsar_pass.h"
#include <bcl/utility.h>
#include <clang/AST/Stmt.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <set>
#include <vector>

namespace clang {
template<class StmtType>
void getStmtTypeFromStmtTree(Stmt *S, std::vector<StmtType *> &VCE,
  std::set<Stmt::StmtClass> Expt = {}) {
  if (auto A = dyn_cast<StmtType>(S))
    VCE.push_back(A);
  for (Stmt *SChild : S->children())
    if (SChild && Expt.find(SChild->getStmtClass()) == Expt.end())
      getStmtTypeFromStmtTree(SChild, VCE, Expt);
}
}

namespace tsar {
struct CalleeProcLocation {
typedef llvm::DenseMap<llvm::Function *,
    std::vector<clang::SourceLocation>> CalleeFuncLoc;
  CalleeProcLocation() {}
  void addCalleeFunc(llvm::Function *F,
      std::vector<clang::SourceLocation> VecSL) {
    mCalleeFuncLoc.insert(std::make_pair(F, VecSL));
  }
  CalleeFuncLoc & getCalleeFuncLoc() {
    return mCalleeFuncLoc;
  }
  void setBreak(std::set<clang::SourceLocation> Break) {
    mBreakStmt = Break;
  }
  std::set<clang::SourceLocation> & getBreak() {
    return mBreakStmt;
  }
  void setReturn(std::set<clang::SourceLocation> Return) {
    mReturnStmt = Return;
  }
  std::set<clang::SourceLocation> & getReturn() {
    return mReturnStmt;
  }
  void setGoto(std::set<clang::SourceLocation> Goto) {
    mGotoStmt = Goto;
  }
  std::set<clang::SourceLocation> & getGoto() {
    return mGotoStmt;
  }
private:
  std::set<clang::SourceLocation> mBreakStmt;
  std::set<clang::SourceLocation> mReturnStmt;
  std::set<clang::SourceLocation> mGotoStmt;
  CalleeFuncLoc mCalleeFuncLoc;
};

typedef llvm::DenseMap<llvm::Function *,
    CalleeProcLocation> FuncCalleeProcInfo;
typedef llvm::DenseMap<clang::Stmt *,
    CalleeProcLocation> StmtCalleeProcInfo;
}

namespace llvm {
class CalleeProcLocationPass :
    public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  CalleeProcLocationPass() : ModulePass(ID) {
    initializeCalleeProcLocationPassPass(*PassRegistry::getPassRegistry());
  }
  tsar::FuncCalleeProcInfo & getFuncCalleeProcInfo() noexcept {
    return mFuncCalleeProcInfo;
  }
  const tsar::FuncCalleeProcInfo &
      getFuncCalleeProcInfo() const noexcept {
    return mFuncCalleeProcInfo;
  }
  tsar::StmtCalleeProcInfo & getLoopCalleeProcInfo() noexcept {
    return mLoopCalleeProcInfo;
  }
  const tsar::StmtCalleeProcInfo &
      getLoopCalleeProcInfo() const noexcept {
    return mLoopCalleeProcInfo;
  }
  bool runOnModule(Module &M) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
private:
  void runOnSCC(CallGraphSCC &SCC, Module &M);
  tsar::FuncCalleeProcInfo mFuncCalleeProcInfo;
  tsar::StmtCalleeProcInfo mLoopCalleeProcInfo;
};
}
