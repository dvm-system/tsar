#include "tsar_pass.h"
#include <bcl/utility.h>
#include <clang/AST/Stmt.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
#include <vector>
#include <set>

namespace {
static const std::set<std::string> SetInOutFunc = {
  "clearerr", "ctermid", "dprintf", "fclose", "fdopen", "feof", "ferror",
  "fflush", "fgetc", "fgetpos", "fgetc", "fgetpos", "fgets", "fileno",
  "flockfile", "fmemopen", "fopen", "fprintf", "fputc", "fputs", "fread",
  "freopen", "fscanf", "fseek", "fseeko", "fsetpos", "ftell", "ftello",
  "ftrylockfile", "funlockfile", "fwrite", "getc", "getchar", "getc_unlocked",
  "getchar_unlocked", "getdelim", "getline", "gets", "open_memstream",
  "pclose", "perror", "popen", "printf", "putc", "putchar", "putc_unlocked",
  "putchar_unlocked", "puts", "remove", "rename", "renameat", "rewind",
  "scanf", "setbuf", "setvbuf", "snprintf", "sprintf", "sscanf", "tempnam",
  "tmpfile", "tmpnam", "tmpfile", "tmpnam", "ungetc", "vdprintf", "vfprintf",
  "vfscanf", "vprintf", "vscanf", "vsprintf", "vsscanf"
};
}

namespace tsar {
struct InterprocElemInfo {
typedef llvm::DenseMap<llvm::Function *,
    std::vector<clang::SourceLocation>> CalleeFuncLoc;
  enum Attr {
    None,
    InOutFunc,
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
private:
  std::set<Attr> mAttrs;
  std::set<clang::SourceLocation> mBreakStmt;
  std::set<clang::SourceLocation> mReturnStmt;
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
