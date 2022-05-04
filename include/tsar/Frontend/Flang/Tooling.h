//===- Tooling.h ----------- Flang Based Tool (Flang) ------------*- C++ -*===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This file implements functions to run flang tools standalone instead of
// running them as a plugin.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FLANG_TOOLING_H
#define TSAR_FLANG_TOOLING_H

#include <clang/Tooling/ArgumentsAdjusters.h>
#include <clang/Tooling/Tooling.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/IntrusiveRefCntPtr.h>
#include <llvm/Support/VirtualFileSystem.h>
#include <string>
#include <vector>

namespace clang::tooling {
class CompilationDatabase;
}

namespace tsar {
class FlangFrontendActionFactory;

class FlangTool {
public:
  FlangTool(const clang::tooling::CompilationDatabase &Compilations,
            llvm::ArrayRef<std::string> SourcePaths,
            llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> BaseFS =
                llvm::vfs::getRealFileSystem())
      : mCompilations(Compilations), mSourcePaths(SourcePaths),
        mOverlayFileSystem(
            new llvm::vfs::OverlayFileSystem(std::move(BaseFS))) {
    appendArgumentsAdjuster(clang::tooling::getClangStripOutputAdjuster());
  }

  ~FlangTool() = default;

  int run(FlangFrontendActionFactory *Factory);

  void setRestoreWorkingDir(bool RestoreCWD) noexcept {
    mRestoreCWD = RestoreCWD;
  }

  void appendArgumentsAdjuster(clang::tooling::ArgumentsAdjuster Adjuster);
  void clearArgumentsAdjusters() { mArgsAdjuster = nullptr; }

private:
  const clang::tooling::CompilationDatabase &mCompilations;
  std::vector<std::string> mSourcePaths;
  llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> mOverlayFileSystem;
  bool mRestoreCWD{true};
  clang::tooling::ArgumentsAdjuster mArgsAdjuster;
};
} // namespace tsar
#endif//TSAR_FLANG_TOOLING_H
