//===- ActionFactory.h --- TSAR Frontend Action (Clang) ---------*- C++ -*-===//
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
// This file proposes general interface to build frontend actions.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ACTION_FACTORY_H
#define TSAR_ACTION_FACTORY_H

#include <bcl/utility.h>
#include <memory>

namespace tsar {
/// Creates an analysis/transformations actions factory.
template <typename FactoryBaseT, typename ActionBaseT, typename ActionT,
          typename... ArgT>
std::unique_ptr<FactoryBaseT> newActionFactory(std::tuple<ArgT...> Args) {
  class ActionFactory : public FactoryBaseT {
  public:
    explicit ActionFactory(std::tuple<ArgT...> Args) : mArgs{std::move(Args)} {}
    std::unique_ptr<ActionBaseT> create() override {
      return std::unique_ptr<ActionBaseT>(
          bcl::make_unique_piecewise<ActionT>(mArgs).release());
    }

  private:
    std::tuple<ArgT...> mArgs;
  };
  return std::unique_ptr<FactoryBaseT>(new ActionFactory(std::move(Args)));
}

/// Creates an analysis/transformations actions factory with adaptor.
template <typename FactoryBaseT, typename ActionBaseT, typename ActionT,
          typename AdaptorT, typename... ActionArgT, typename... AdaptorArgT>
std::unique_ptr<FactoryBaseT>
newActionFactory(std::tuple<ActionArgT...> ActionArgs = {},
                 std::tuple<AdaptorArgT...> AdaptorArgs = {}) {
  class ActionFactory : public FactoryBaseT {
  public:
    ActionFactory(std::tuple<ActionArgT...> ActionArgs,
                  std::tuple<AdaptorArgT...> AdaptorArgs)
        : mActionArgs{std::move(ActionArgs)}, mAdaptorArgs{
                                                  std::move(AdaptorArgs)} {}
    std::unique_ptr<ActionBaseT> create() override {
      std::unique_ptr<ActionBaseT> Action{
          bcl::make_unique_piecewise<ActionT>(mActionArgs).release()};
      return std::unique_ptr<ActionBaseT>(
          bcl::make_unique_piecewise<AdaptorT>(
              std::tuple_cat(std::forward_as_tuple(std::move(Action)),
                             mAdaptorArgs))
              .release());
    }

  private:
    std::tuple<ActionArgT...> mActionArgs;
    std::tuple<AdaptorArgT...> mAdaptorArgs;
  };
  return std::unique_ptr<FactoryBaseT>(
      new ActionFactory(std::move(ActionArgs), std::move(AdaptorArgs)));
}
} // namespace tsar
#endif//TSAR_ACTION_FACTORY_H
