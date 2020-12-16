//===- PassAAProvider.h -- On The Fly AA Passes Provider --------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
// This file defines a provider which provides alias analysis results for
// required passes.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PASS_AA_PROVIDER_H
#define TSAR_PASS_AA_PROVIDER_H

#include "tsar/Analysis/Memory/AllocasModRef.h"
#include "tsar/Support/AnalysisWrapperPass.h"
#include "tsar/Support/PassProvider.h"
#include <llvm/Analysis/AliasAnalysis.h>
#include <llvm/Analysis/CFLAndersAliasAnalysis.h>
#include <llvm/Analysis/CFLSteensAliasAnalysis.h>
#include <llvm/Analysis/GlobalsModRef.h>
#include <llvm/Analysis/ScopedNoAliasAA.h>
#include <llvm/Analysis/TypeBasedAliasAnalysis.h>

namespace llvm {
/// Initialize a pass to access results of alias analysis for globals
/// from on the fly passes.
void initializeGlobalsAAResultImmutableWrapperPass(PassRegistry &);
}

namespace {
/// This allow to access results of alias analysis for globals from on the fly
/// passes.
using GlobalsAAResultImmutableWrapper =
    llvm::AnalysisWrapperPass<llvm::GlobalsAAResult>;

// This provider allow to access alias analysis results from on the fly passes.
template <class... Analysis>
class FunctionPassAAProvider
    : public FunctionPassProvider<
          llvm::CFLSteensAAWrapperPass, llvm::CFLAndersAAWrapperPass,
          llvm::TypeBasedAAWrapperPass, llvm::ScopedNoAliasAAWrapperPass,
          llvm::AllocasAAWrapperPass, Analysis...> {
  void preparePassManager(llvm::PMStack &PMS) override {
    assert(PMS.top()->getPassManagerType() == llvm::PMT_FunctionPassManager &&
           "Top manager for on the flay passes must be of a function kind!");
    initializeGlobalsAAResultImmutableWrapperPass(
        *llvm::PassRegistry::getPassRegistry());
    auto *PM = PMS.top()->getTopLevelManager();
    PM->schedulePass(new GlobalsAAResultImmutableWrapper);
    PM->schedulePass(llvm::createExternalAAWrapperPass(
        [](llvm::Pass &P, llvm::Function &F, llvm::AAResults &AAR) {
          if (auto *WrapperPass =
                  P.getAnalysisIfAvailable<GlobalsAAResultImmutableWrapper>())
            if (*WrapperPass)
              AAR.addAAResult(WrapperPass->get());
          if (auto *AAP =
                  P.getAnalysisIfAvailable<llvm::AllocasAAWrapperPass>()) {
            AAP->getResult().analyzeFunction(F);
            AAR.addAAResult(AAP->getResult());
          }
        }));
  }
};
} // namespace

#endif//TSAR_PASS_AA_PROVIDER_H
