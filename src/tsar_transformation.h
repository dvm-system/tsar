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
class ASTContext;
}

namespace tsar {
/// \brief A prototype of a filename adjuster.
///
/// Filename adjuster is responsible for modification of filenames before the
/// files are used to writer or read data.
typedef std::function<std::string(llvm::StringRef)> FilenameAdjuster;

/// Returns a filename adjuster which does not modify name of files.
inline FilenameAdjuster getPureFilenameAdjuster() {
  return [](llvm::StringRef Filename) { return Filename; };
}

/// \brief Returns a filename adjuster which generates the following name:
/// name.extension -> name.number.extension.
///
/// For the same file during a single program work session the 'number' is
/// increased every time the adjuster is called. For different work sessions
/// the countdown begins again and the files will be overwritten.
FilenameAdjuster getDumpFilenameAdjuster();

/// Returns a filename adjuster which generates the following name:
/// name.extension -> name.extension.orig
inline FilenameAdjuster getBackupFilenameAdjuster() {
  return [](llvm::StringRef Filename) -> std::string {
    return (Filename + ".orig").str();
  };
}

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
/// can be reseted multiple times.
class TransformationContext {
public:
  /// \brief Constructor.
  ///
  /// \param [in] CL Command line which is necessary to parse input file.
  /// \post Transformation engine is NOT configured yet.
  explicit TransformationContext(llvm::ArrayRef<std::string> CL);

  /// \brief Constructor.
  ///
  /// \param [in] CL Command line which is necessary to parse input file.
  /// \post Transformation engine is configured.
  TransformationContext(clang::ASTContext &Ctx, clang::CodeGenerator &Gen,
    llvm::ArrayRef<std::string> CL);

  /// \brief Returns an input source file.
  ///
  /// \pre Transformation instance must be configured.
  llvm::StringRef getInput() const;

  /// Returns command line to parse a source file.
  llvm::ArrayRef<std::string> getCommandLine() const { return mCommandLine; }

  /// Returns rewriter.
  clang::Rewriter & getRewriter() {
    assert(hasInstance() && "Rewriter is not configured!");
    return mRewriter;
  }

  /// Returns context.
  clang::ASTContext & getContext() {
    assert(hasInstance() && "Rewriter is not configured!");
    return *mCtx;
  }

  /// \brief Returns a declaration for a mangled name.
  ///
  /// \pre Transformation instance must be configured.
  clang::Decl * getDeclForMangledName(llvm::StringRef Name);

  /// Returns true if transformation engine is configured.
  bool hasInstance() const { return mGen && mCtx; }

  /// Returns true if modifications have been made to some files.
  bool hasModification() const {
    return hasInstance() &&
      mRewriter.buffer_begin() != mRewriter.buffer_end();
  }

  /// \brief Save all changed files to disk.
  ///
  /// \param FA Filename adjuster which modifies names of files where the
  /// changes must be saved. The parameter for an adjuster is a name of a
  /// modified file.
  /// \return Pair of values. The first value is the name of file where main
  /// input file has been saved. The second value is 'true' if all changes
  /// have been successfully saved.
  std::pair<std::string, bool> release(
    const FilenameAdjuster &FA = getDumpFilenameAdjuster()) const;

  /// Save all changes in a specified buffer to disk (in file with a specified
  /// name), emits diagnostic messages in case of error.
  void release(llvm::StringRef Filename, const clang::RewriteBuffer &Buffer);

  /// \brief Resets existence configuration of transformation engine.
  ///
  /// \post Transformation engine is configured.
  void reset(clang::ASTContext &Ctx, clang::CodeGenerator &Gen,
      llvm::ArrayRef<std::string> CL);

  /// \brief Resets existence configuration of transformation engine.
  ///
  /// \post Transformation engine is configured.
  void reset(clang::ASTContext &Ctx, clang::CodeGenerator &Gen);

  /// \brief Resets existence configuration of transformation engine.
  ///
  /// \post Transformation engine is NOT configured.
  void reset() { mGen = nullptr; mCtx = nullptr; }

private:
  clang::Rewriter mRewriter;
  clang::CodeGenerator *mGen;
  clang::ASTContext *mCtx;
  std::vector<std::string> mCommandLine;
};
}

namespace llvm {
class Module;

/// Initializes rewriter to update source code of the specified module.
class TransformationEnginePass :
  public ImmutablePass, private bcl::Uncopyable {
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
  void setContext(llvm::Module &M, tsar::TransformationContext *Ctx) {
    auto Pair = mTransformCtx.insert(std::make_pair(&M, Ctx));
    if (!Pair.second)
      Pair.first->second = Ctx;
  }

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
