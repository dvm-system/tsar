//===- TransformationContext.h - TSAR Transformation Engine -----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file defines source level transformation engine which is necessary to
// transform high level and low-level representation of program correlated.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_TRANSFORMATION_ENGINE_H
#define TSAR_TRANSFORMATION_ENGINE_H

#include <tsar/Support/AnalysisWrapperPass.h>
#include <bcl/tagged.h>
#include <bcl/utility.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/Twine.h>
#include <llvm/Pass.h>
#include <llvm/Support/raw_ostream.h>
#include <functional>
#include <optional>
#include <variant>

namespace clang {
namespace tooling {
class CompilationDatabase;
}
} // namespace clang

namespace llvm {
class DICompileUnit;
class Module;
}

namespace tsar {
/// A wrapper for a file stream that atomically overwrites the target.
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
  using Args2T = std::tuple<std::string, std::string>;
  using Args3T = std::tuple<std::string, std::string, std::string>;
  using ErrorArgsT = std::variant<Args2T, Args3T>;
  using ErrorT = std::optional<std::tuple<unsigned, ErrorArgsT>>;

  /// Creates a file output stream for a temporary file which is later
  /// accessible via getStream() if ok() return true.
  ///
  /// Specify `Error` to access error description if any.
  AtomicallyMovedFile(llvm::StringRef File, ErrorT *Error = nullptr);

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
  llvm::StringRef mFilename;
  ErrorT *mError;
  llvm::SmallString<128> mTempFilename;
  std::unique_ptr<llvm::raw_fd_ostream> mFileStream;
  bool mUseTemporary;
};

/// Filename adjuster is responsible for modification of filenames before the
/// files are used to writer or read data.
typedef std::function<std::string(llvm::StringRef)> FilenameAdjuster;

/// Return a filename adjuster which does not modify name of files.
inline FilenameAdjuster getPureFilenameAdjuster() {
  return [](llvm::StringRef Filename) { return Filename.str(); };
}

/// Return a filename adjuster which generates the following name:
/// name.extension -> name.number.extension.
///
/// For the same file during a single program work session the 'number' is
/// increased every time the adjuster is called. For different work sessions
/// the counting begins again and the files will be overwritten.
FilenameAdjuster getDumpFilenameAdjuster();

/// Return a filename adjuster which generates the following name:
/// name.extension -> name.extension.orig
inline FilenameAdjuster getBackupFilenameAdjuster() {
  return [](llvm::StringRef Filename) -> std::string {
    return (Filename + ".orig").str();
  };
}

/// This class represents state of the current source level
/// transformation engine.
///
/// A single configured instance of this class associated with a single source
/// file (with additional include files) and command line options which is
/// necessary to parse this file before it would be rewritten.
class TransformationContextBase
    : public llvm::ThreadSafeRefCountedBase<TransformationContextBase>,
      private bcl::Uncopyable {
public:
  enum Kind : uint8_t {
    TC_Flang,
    TC_Clang,
  };

  virtual ~TransformationContextBase() {}

  /// Returns true if transformation engine is configured.
  virtual bool hasInstance() const = 0;

  /// Returns true if modifications have been made to some files.
  virtual bool hasModification() const = 0;

  /// Save all changed files to disk.
  ///
  /// \param FA Filename adjuster which modifies names of files where the
  /// changes must be saved. The parameter for an adjuster is a name of a
  /// modified file.
  /// \return Pair of values. The first value is the name of file where main
  /// input file has been saved. The second value is 'true' if all changes
  /// have been successfully saved.
  virtual std::pair<std::string, bool>
  release(const FilenameAdjuster &FA = getDumpFilenameAdjuster()) = 0;

  auto getKind() const noexcept { return mKind; }

protected:
  explicit TransformationContextBase(Kind K) : mKind(K) {}

private:
  Kind mKind;
};

/// Storage for all available transformation contexts.
class TransformationInfo final : private bcl::Uncopyable {
  using TransformationMap =
      llvm::DenseMap<llvm::DICompileUnit *,
                     llvm::IntrusiveRefCntPtr<tsar::TransformationContextBase>>;

public:
  using value_type =
      bcl::tagged_pair<bcl::tagged<llvm::DICompileUnit *, llvm::DICompileUnit>,
                       bcl::tagged<tsar::TransformationContextBase *,
                                   tsar::TransformationContextBase>>;

private:
  struct iterator_helper {
    value_type &operator() ( TransformationMap::value_type &V ) const {
      Storage = value_type{ V.first, V.second.get() };
      return Storage;
    }
    mutable value_type Storage;
  };

public:
  using iterator =
      llvm::mapped_iterator<TransformationMap::iterator, iterator_helper>;

  /// Create storage for transformation contexts.
  explicit TransformationInfo(
      const clang::tooling::CompilationDatabase &Compilations)
      : mCompilations(Compilations) {}

  /// Returns command line which has been used to run analysis.
  const clang::tooling::CompilationDatabase &
  getCompilationDatabase() const noexcept {
    return mCompilations;
  }

  /// Set transformation context for a specified compilation unit.
  void setContext(llvm::DICompileUnit &CU,
      llvm::IntrusiveRefCntPtr<TransformationContextBase> Ctx) {
    mTransformPool.try_emplace(&CU, std::move(Ctx));
  }

  /// Return transformation context for a specified compilation unit or nullptr.
  tsar::TransformationContextBase *getContext(llvm::DICompileUnit &CU) {
    auto I = mTransformPool.find(&CU);
    return I != mTransformPool.end() ? I->second.get() : nullptr;
  }

  bool empty() const { return mTransformPool.empty(); }

  auto contexts() {
    return llvm::make_range(iterator{mTransformPool.begin(), iterator_helper{}},
                            iterator{mTransformPool.end(), iterator_helper{}});
  }

private:
  TransformationMap mTransformPool;
  const clang::tooling::CompilationDatabase &mCompilations;
};
} // namespace tsar

namespace llvm {
/// Initialize a pass to access source level transformation enginer.
void initializeTransformationEnginePassPass(PassRegistry &Registry);

/// Create a pass to access source level transformation enginer.
ImmutablePass * createTransformationEnginePass();

/// Immutable pass to store data necessary for source-level transformation.
using TransformationEnginePass = AnalysisWrapperPass<tsar::TransformationInfo>;
}
#endif//TSAR_TRANSFORMATION_ENGINE_H
