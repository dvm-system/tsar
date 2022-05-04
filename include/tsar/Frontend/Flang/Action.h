//===- Action.h -------- TSAR Frontend Action (Flang) ------------*- C++ -*===//
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
// This file contains front-end actions which are necessary to analyze and
// transform sources.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_FLANG_ACTION_H
#define TSAR_FLANG_ACTION_H

#include "tsar/Core/TransformationContext.h"
#include "tsar/Frontend/ActionFactory.h"
#include <flang/Frontend/FrontendActions.h>
#include <llvm/ADT/StringRef.h>
#include <string>

namespace tsar {
class QueryManager;

class FlangFrontendAction : public Fortran::frontend::CodeGenAction {
public:
  FlangFrontendAction()
      : Fortran::frontend::CodeGenAction(
            Fortran::frontend::BackendActionTy::Backend_EmitLL) {}
  bool hasWorkingDir() const { return !mWorkingDir.empty(); }
  llvm::StringRef getWorkingDir() const { return mWorkingDir; }
  void setWorkingDir(llvm::StringRef Path) { mWorkingDir = Path; }

private:
  std::string mWorkingDir;
};

class FlangMainAction : public FlangFrontendAction {
public:
  FlangMainAction(const clang::tooling::CompilationDatabase &Compilations,
                  QueryManager &QM)
      : mTfmInfo(Compilations), mQueryManager(QM) {}

  void executeAction() override;
  bool shouldEraseOutputFiles() override;
  bool beginSourceFileAction() override;

private:
  TransformationInfo mTfmInfo;
  QueryManager &mQueryManager;
};

class FlangFrontendActionFactory {
public:
  virtual ~FlangFrontendActionFactory() = default;
  virtual std::unique_ptr<FlangFrontendAction> create() = 0;
};

/// Creates an analysis/transformations actions factory.
template <typename ActionT, typename... ArgT>
std::unique_ptr<FlangFrontendActionFactory>
newFlangActionFactory(std::tuple<ArgT...> Args) {
  return newActionFactory<FlangFrontendActionFactory,
                          FlangFrontendAction, ActionT>(
      std::move(Args));
}

/// Creates an analysis/transformations actions factory with adaptor.
template <typename ActionT, typename AdaptorT, typename... ActionArgT,
          typename... AdaptorArgT>
std::unique_ptr<FlangFrontendActionFactory>
newFlangActionFactory(std::tuple<ActionArgT...> ActionArgs = {},
                      std::tuple<AdaptorArgT...> AdaptorArgs = {}) {
  return newActionFactory<FlangFrontendActionFactory,
                          FlangFrontendAction, ActionT, AdaptorT>(
      std::move(ActionArgs), std::move(AdaptorArgs));
}
} // namespace tsar
#endif // TSAR_FLANG_ACTION_H
