#include "tsar_pass.h"
#include <bcl/utility.h>
#include <clang/AST/Stmt.h>
#include <llvm/Analysis/CallGraphSCCPass.h>
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
struct InterprocAttrs {
  enum Attr {
    None,
    InOutFunc,
    NoReturn,
    EndAttr
  };
  InterprocAttrs() {}
  void setAttr(Attr Type) {
    mAttrs.insert(Type);
  }
  bool hasAttr(Attr Type) {
    return mAttrs.find(Type) != mAttrs.end();
  }
  private:
  std::set<Attr> mAttrs;
};
typedef llvm::DenseMap<clang::Stmt *,
    InterprocAttrs> InterprocAttrStmtInfo;
}

namespace llvm {
  class InterprocAttrPass :
    public ModulePass, private bcl::Uncopyable {
  public:
    static char ID;
    InterprocAttrPass() : ModulePass(ID) {
      initializeInterprocAttrPassPass(*PassRegistry::getPassRegistry());
    }
    tsar::InterprocAttrStmtInfo & getInterprocAttrLoopInfo() noexcept {
      return mInterprocAttrLoopInfo;
    }
    const tsar::InterprocAttrStmtInfo &
        getInterprocAttrLoopInfo() const noexcept {
      return mInterprocAttrLoopInfo;
    }
    bool runOnModule(Module &M) override;
    void getAnalysisUsage(AnalysisUsage &AU) const override;
  private:
    void runOnSCC(CallGraphSCC &SCC, Module &M);
    tsar::InterprocAttrStmtInfo mInterprocAttrLoopInfo;
  };
}
