//=== InstructionMatcher.h - High and Low Level Instruction Matcher -- C++ ===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2021 DVM System Group
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
// Classes and functions from this file match AST statements in a high-level
// code and appropriate instructions in low-level LLVM IR. This file implements
// pass to perform this functionality.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_INSTRUCTION_MATCHER_H
#define TSAR_INSTRUCTION_MATCHER_H

#include "tsar/ADT/Bimap.h"
#include "tsar/Analysis/Clang/Passes.h"
#include <bcl/utility.h>
#include <llvm/Pass.h>
#include <set>

#include "tsar/Support/Tags.h"

namespace clang {
  class Stmt;
}

namespace llvm {
  class Instruction;

  /// \brief This function pass makes loop distribution transformation.
  class InstructionMatcherPass : public FunctionPass, private bcl::Uncopyable {
  public:
    typedef tsar::Bimap<
      bcl::tagged<clang::Stmt *, tsar::AST>,
      bcl::tagged<Instruction *, tsar::IR>> InstructionMatcher;
    typedef std::set<clang::Stmt*> StmtASTSet;

    /// Pass identification, replacement for typeid.
    static char ID;

    /// Default constructor.
    InstructionMatcherPass() : FunctionPass(ID) {
      initializeInstructionMatcherPassPass(
        *PassRegistry::getPassRegistry());
    }

    /// Makes loop distribution transformation.
    bool runOnFunction(Function& F) override;

    /// Specifies a list of analyzes that are necessary for this pass.
    void getAnalysisUsage(AnalysisUsage& AU) const override;

    /// \brief Returns instruction matcher for the last analyzed function.
    const InstructionMatcher& getMatcher() const noexcept { return mMatcher; }

    /// \brief Returns unmatched AST statements.
    const StmtASTSet& getUnmatchedAST() const noexcept { return mUnmatchedAST; }

    /// Releases allocated memory.
    void releaseMemory() override {
      mMatcher.clear();
      mUnmatchedAST.clear();
    }

  private:
    InstructionMatcher mMatcher;
    StmtASTSet mUnmatchedAST;
  };
}

#endif//TSAR_INSTRUCTION_MATCHER_H