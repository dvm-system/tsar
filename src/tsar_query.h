//===------ tsar_query.h --------- Query Manager ----------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This defines a query manager that controls construction of response when
// analysis and transformation tool is launched.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_QUERY_H
#define TSAR_QUERY_H

#include <llvm/ADT/StringRef.h>
#include <llvm/Config/llvm-config.h>

namespace llvm {
class Pass;
class Module;
class raw_pwrite_stream;
}

namespace clang {
class CompilerInstance;
class CodeGenOptions;
}

namespace tsar {
class TransformationContext;

/// This is a query manager that controls construction of response when analysis
/// and transformation tool is launched.
///
/// This describes default behavior of a tool. Tow ways are available to
/// override it:
/// - The simplest way is to override createInitializationPass() and
/// createFinalizationPass() methods. Passes constructed by these methods are
/// launched before and after default sequence of passes.
/// - The second way is to override whole sequence of performed passes.
class QueryManager {
public:
  /// \brief Callback at the start of processing a single input.
  ///
  /// \return True on success, on failure run() passes will not be called.
  virtual bool beginSourceFile(clang::CompilerInstance &, llvm::StringRef) {
    return true;
  }

  /// \brief Callback at the end of processing a single input.
  ///
  /// This is guaranteed to only be called following a successful call to
  /// beginSourceFile().
  virtual void endSourceFile() {}

  /// Creates an initialization pass to respond a query.
  virtual llvm::Pass * createInitializationPass() { return nullptr; }

  /// Creates a finalization pass to respond a query.
  virtual llvm::Pass * createFinalizationPass() { return nullptr; }

  /// Analysis the specified module and transforms source file associated with
  /// it if rewriter context is specified.
  ///
  /// \attention Neither module nor transformation context is not going to be
  /// taken under control.
  virtual void run(llvm::Module *M, tsar::TransformationContext *Ctx);
};


/// This prints LLVM IR to the standard output stream.
class EmitLLVMQueryManager : public QueryManager {
public:
  bool beginSourceFile(
    clang::CompilerInstance &CI, llvm::StringRef InFile) override;
  void run(llvm::Module *M, tsar::TransformationContext *) override;
private:
#if (LLVM_VERSION_MAJOR < 4)
  llvm::raw_pwrite_stream *mOS = nullptr;
#else
  std::unique_ptr<llvm::raw_pwrite_stream > mOS;
#endif
  const clang::CodeGenOptions *mCodeGenOpts = nullptr;
};

/// This performs instrumentation of LLVM IR and prints it to the standard
/// output stream after instrumentation.
class InstrLLVMQueryManager : public EmitLLVMQueryManager {
  llvm::Pass * createInitializationPass() override;
};
}

#endif//TSAR_QUERY_H
