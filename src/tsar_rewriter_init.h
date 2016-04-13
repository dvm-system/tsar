#ifndef TSAR_REWRITER_INIT_H
#define TSAR_REWRITER_INIT_H

#include <clang/Basic/SourceManager.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm\Pass.h>
#include <functional>
#include <utility.h>

namespace clang {
class Decl;
}

namespace tsar {
/// This is used to receive declaration on behalf of the mangled name.
typedef std::function<clang::Decl * (llvm::StringRef)> Demangler;

class RewriterContext;
}

namespace llvm {
class Module;

/// Intializes rewriter to update source code of the specified module.
class RewriterInitializationPass :
  public ModulePass, private Utility::Uncopyable {
  typedef llvm::DenseMap<llvm::Module *, tsar::RewriterContext *> RewriterMap;

public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  RewriterInitializationPass() : ModulePass(ID) {
    initializeRewriterInitializationPassPass(*PassRegistry::getPassRegistry());
  }

  /// Destructor.
  ~RewriterInitializationPass() {
    for (auto &Pair : mRewriterCtx)
      delete Pair.second;
    mRewriterCtx.clear();
  }

  /// Initializes rewriter context for the specified module.
  ///
  /// This function must be called for each analyzed module before execution of
  /// a pass manager. If data to build rewriter instance is available
  /// (see RewriterContext::hasInstance()) re-parsing of source code which is
  /// associated with the specified module will be prevented. If this function
  /// is called several times it accumulates information for different modules.
  /// So it is possible to prevent re-parsing of multiple sources.
  /// \attention The pass retains ownership of the context, so do not delete it
  /// from the outside.
  void setRewriterContext(llvm::Module &M, tsar::RewriterContext *Ctx) {
    mRewriterCtx.insert(std::make_pair(&M, Ctx));
  }

  /// Intializes rewriter to update source code of the specified module.
  bool runOnModule(llvm::Module &M) override;

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  void releaseMemory() override;

  /// Returns rewriter for the last evaluated module.
  clang::Rewriter & getRewriter() { return mRewriter; }

  /// Returns declaration in the last evaluated module which is associated with
  /// the specified mangled name.
  clang::Decl * getDeclForMangledName(StringRef MN) { return mDemangler(MN); }

private:
  clang::Rewriter mRewriter;
  tsar::Demangler mDemangler;
  RewriterMap mRewriterCtx;
};
}
#endif//TSAR_REWRITER_INIT_H
