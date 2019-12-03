//===--- DefinedMemory.cpp --- Defined Memory Analysis ----------*- C++ -*-===//
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
#ifndef TSAR_SPLIT_BASIC_BLOCK_WITH_CALL_INST_H
#define TSAR_SPLIT_BASIC_BLOCK_WITH_CALL_INST_H

#include <tsar/Transform/IR/Passes.h>
#include <llvm/Pass.h>
#include <bcl/utility.h>

namespace llvm {
class SplitBasicBlocksWithCallInstPass: public FunctionPass, private bcl::Uncopyable {
public:
  static char ID;
  SplitBasicBlocksWithCallInstPass() :FunctionPass(ID) {
    initializeSplitBasicBlocksWithCallInstPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  /// Specifies a list of analyzes  that are necessary for this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

};
}
#endif//TSAR_SPLIT_BASIC_BLOCK_WITH_CALL_INST_H
