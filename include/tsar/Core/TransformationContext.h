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
#include <bcl/utility.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallString.h>
#include <llvm/ADT/Twine.h>
#include <llvm/Pass.h>
#include <functional>

namespace llvm {
class DICompileUnit;
class Module;
}

namespace tsar {
class ClangTransformationContext;

/// A prototype of a filename adjuster.
///
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
class TransformationContextBase : private bcl::Uncopyable {
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
  release(const FilenameAdjuster &FA = getDumpFilenameAdjuster()) const = 0;

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
                     std::unique_ptr<tsar::TransformationContextBase>>;

public:
  /// Create storage for transformation contexts.
  ///
  /// \param [in] CL Command line which has been used to run analysis.
  explicit TransformationInfo(llvm::ArrayRef<std::string> CL)
    : mCommandLine(CL) {}

  /// Returns command line which has been used to run analysis.
  llvm::ArrayRef<std::string> getCommandLine() const { return mCommandLine; }

  /// Return transformation context for the module or null.
  [[deprecated("use getContext(llvm::DICompileUnit &)")]]
  tsar::ClangTransformationContext *getContext(llvm::Module &M);

  /// Set transformation context for a specified compilation unit.
  void setContext(llvm::DICompileUnit &CU,
      std::unique_ptr<TransformationContextBase> &&Ctx) {
    mTransformPool.try_emplace(&CU, std::move(Ctx));
  }

  /// Return transformation context for a specified compilation unit or nullptr.
  tsar::TransformationContextBase *getContext(llvm::DICompileUnit &CU) {
    auto I = mTransformPool.find(&CU);
    return I != mTransformPool.end() ? I->second.get() : nullptr;
  }

  bool empty() const { return mTransformPool.empty(); }

private:
  TransformationMap mTransformPool;
  std::vector<std::string> mCommandLine;
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
