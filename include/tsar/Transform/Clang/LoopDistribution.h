//===-- LoopDistribution.h - Source-level Loop Distribution (Clang) - C++ -===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file declares a pass that makes loop distribution transformation.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_LOOP_DISTRIBUTION_H
#define TSAR_LOOP_DISTRIBUTION_H

#include "tsar/Transform/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>

namespace llvm {
    /// \brief This function pass makes loop distribution transformation.
    class LoopDistributionPass : public FunctionPass, private bcl::Uncopyable {
    public:
        /// Pass identification, replacement for typeid.
        static char ID;

        /// Default constructor.
        LoopDistributionPass() : FunctionPass(ID) {
            initializeLoopDistributionPassPass(
                *PassRegistry::getPassRegistry());
        }

        /// Makes loop distribution transformation.
        bool runOnFunction(Function& F) override;

        /// Specifies a list of analyzes that are necessary for this pass.
        void getAnalysisUsage(AnalysisUsage& AU) const override;
    };
}
#endif// TSAR_LOOP_DISTRIBUTION_H