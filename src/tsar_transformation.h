//===- tsar_transformation.h - TSAR Transformation Engine--------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// This file defines source level transformation engine which is necessary to
// transform high level and low-level representation of program correlated.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_TRANSFORMATION_ENGINE_H
#define TSAR_TRANSFORMATION_ENGINE_H

#include <clang/Basic/SourceManager.h>
#include <clang/Rewrite/Core/Rewriter.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/Pass.h>
#include <functional>
#include <utility.h>
#include "tsar_pass.h"

namespace clang {
class Decl;
class CodeGenerator;
}

namespace tsar {
/// \brief A prototype of a filename adjuster.
///
/// Filename adjuster is responsible for modification of filenames before the
/// files are used to writer or read data.
typedef std::function<std::string(llvm::StringRef)> FilenameAdjuster;

/// Returns a filename adjuster wich does not moidfy name of files.
inline FilenameAdjuster getPureFilenameAdjuster() {
  return [](llvm::StringRef Filename) { return Filename; };
}

/// \brief Returns a filename adjuster which generate the following name:
/// name.extnsion -> name.number.extension.
///
/// For the same file the 'number' is increased every time the adjuster is
/// called.
FilenameAdjuster getDumpFilenameAdjuster();

/// \brief A wrapper for a file stream that atomically overwrites the target.
///
/// Creates a file output stream for a temporary file in the constructor,
/// which is later accessible via getStream() if ok() return true.
/// Flushes the stream and moves the temporary file to the target location
/// in the destructor.
/// \note If temporary file can not be created, it try to write original file
/// directly.
class AtomicallyMovedFile {
  /// Checks that file can be written and that temporary file can be used.
  ///
  /// \return False if file can not be written. In this case appropriate
  /// diagnostics will be written.
  bool checkStatus();

public:
  /// Creates a file output stream for a temporary file which is later
  /// accessible via getStream() if ok() return true.
  AtomicallyMovedFile(clang::DiagnosticsEngine &DE, llvm::StringRef File);

  /// Flushes the stream and moves the temporary file to the target location.
  ~AtomicallyMovedFile();

  /// Returns true if a file stream has been successfully created.
  bool hasStream() { return static_cast<bool>(mFileStream); }

  /// Returns created file stream.
  llvm::raw_ostream &getStream() {
    assert(hasStream() && "File stream is not available!");
    return *mFileStream;
  }

private:
  clang::DiagnosticsEngine &mDiagnostics;
  llvm::StringRef mFilename;
  llvm::SmallString<128> mTempFilename;
  std::unique_ptr<llvm::raw_fd_ostream> mFileStream;
  bool mUseTemporary;
};

/// \brief This class represents state of the current source level
/// transformation engine.
///
/// A single configured instance of this class associated with a single source
/// file (with additional include files) and command line options which is
/// necessary to parse this file before it would be rewritten. The configuration
/// can be reseted reseted multiple times.
class TransformationContext {
public:
  /// Constructor.
  TransformationContext(clang::ASTContext &Ctx, clang::CodeGenerator &Gen,
    llvm::ArrayRef<std::string> CL);

  /// Returns an input source file.
  llvm::StringRef getInput() const;

  /// Returns command line to parse a source file.
  llvm::ArrayRef<std::string> getCommandLine() const { return mCommandLine; }

  /// Returns rewriter.
  clang::Rewriter & getRewriter() {
    assert(hasInstance() && "Rewriter is not configured!");
    return mRewriter;
  }

  /// Returns a declaration for a mangled name.
  clang::Decl * getDeclForMangledName(llvm::StringRef Name);

  /// Returns true if transformation engine is configured.
  bool hasInstance() const { return mGen; }

  /// Returns true if modifications have been made to some files.
  bool hasModification() const {
    return hasInstance() &&
      mRewriter.buffer_begin() != mRewriter.buffer_end();
  }

  /// \brief Save all changed files to disk.
  ///
  /// \param FA Filename adjuster wich modifies names of files where the changes
  /// must be saved. The parameter for an adjuster is a name of a modified file.
  /// \return Pair of values. The first value is the name of file where main
  /// input file has been saved. The second value is 'true' if all changes
  /// have been successfully saved.
  std::pair<std::string, bool> release(
    FilenameAdjuster FA = getDumpFilenameAdjuster()) const;

  /// Resets existence configuration of transformation engine.
  void reset(clang::ASTContext &Ctx, clang::CodeGenerator &Gen,
      llvm::ArrayRef<std::string> CL);

  /// Resets existence configuration of transformation engine.
  void reset(clang::ASTContext &Ctx, clang::CodeGenerator &Gen);

  /// Resets existence configuration of transformation engine.
  void reset() { mGen = nullptr; }

private:
  clang::Rewriter mRewriter;
  clang::CodeGenerator *mGen;
  std::vector<std::string> mCommandLine;
};
}

namespace llvm {
class Module;

/// Intializes rewriter to update source code of the specified module.
class TransformationEnginePass :
  public ImmutablePass, private Utility::Uncopyable {
  typedef llvm::DenseMap<llvm::Module *, tsar::TransformationContext *>
    TransformationMap;

public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  TransformationEnginePass() : ImmutablePass(ID) {
    initializeTransformationEnginePassPass(*PassRegistry::getPassRegistry());
  }

  /// Destructor.
  ~TransformationEnginePass() { releaseMemory(); }

  /// Set transformation context for the specified module.
  ///
  /// This function must be called for each analyzed module before execution of
  /// a pass manager. If transformation engine is configured
  /// (see TransformationContext::hasInstance()) re-parsing of source code which
  /// is associated with the specified module will be prevented. If this
  /// function is called several times it accumulates information for different
  ///  modules. So it is possible to prevent re-parsing of multiple sources.
  /// \attention The pass retains ownership of the context, so do not delete it
  /// from the outside.
  void setContext(llvm::Module &M, tsar::TransformationContext *Ctx) {
    mTransformCtx.insert(std::make_pair(&M, Ctx));
  }

  /// Releases all transformation contexts.
  void releaseMemory() override;

  /// Returns transformation context for the module or null.
  tsar::TransformationContext * getContext(llvm::Module &M) {
    auto I = mTransformCtx.find(&M);
    return I != mTransformCtx.end() ? I->second : nullptr;
  }

private:
  TransformationMap mTransformCtx;
};
}
#endif//TSAR_TRANSFORMATION_ENGINE_H
