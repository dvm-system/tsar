//===--- PrivateAnalysis.h - Private Variable Analyzer ----------*- C++ -*-===//
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
// This file defines passes to determine formal arguments which can be
// preserved by the function (e.g. saved to the global memory).
//
//===----------------------------------------------------------------------===//

#ifndef SAPFOR_ADDRESSACCESS_H
#define SAPFOR_ADDRESSACCESS_H

#include "tsar/Analysis/Memory/Passes.h"
#include <llvm/Pass.h>
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"

namespace tsar {
  struct PreservedParametersInfo {
    using StoredPtrArguments = llvm::DenseSet<int>;
    using FunctionToArguments = llvm::DenseMap<
            const llvm::Function *, StoredPtrArguments *>;

    FunctionToArguments infoByFun;
    bool failed;  /// set if analysis has failed
    PreservedParametersInfo() :
      infoByFun(FunctionToArguments()), failed(false) {};

    void addFunction(llvm::Function *F) {
      infoByFun[F] = new StoredPtrArguments();
    }
  };
}

namespace llvm {
  struct InstrDependencies {
    std::vector<Instruction *> deps;
    bool is_invalid;

    InstrDependencies() :
            deps(std::vector<Instruction *>()), is_invalid(false) {};

    explicit InstrDependencies(bool is_invalid) : deps(
            std::vector<Instruction *>()), is_invalid(is_invalid) {};
  };

  class AddressAccessAnalyser :
          public ModulePass, private bcl::Uncopyable {

    using ValueSet = DenseSet<llvm::Value *>;
    /// maps instructions on those which are dependent on them
    using DependentInfo = DenseMap<Instruction *, InstrDependencies *>;
    using AccessInfo = tsar::AccessInfo;
    static bool isNonTrivialPointerType(Type *);
    Function *CurFunction = nullptr;
    AAResults* AA = nullptr;
  public:
    static char ID;

    AddressAccessAnalyser() : ModulePass(ID) {
      initializeAddressAccessAnalyserPass(*PassRegistry::getPassRegistry());
    }

    tsar::PreservedParametersInfo &getAA() { return mParameterAccesses; };

    void setNocaptureToAll(Function* );

    bool isStored(Value *);

    /// initializes mDepInfo
    void initDepInfo(Function *);

    /// returns values which may contain incoming value as a result
    /// of execution instruction
    std::vector<Value *> useMovesTo(Value *, Instruction *, bool &);

    /// tries to construct tree of local usages of the argument's value
    /// returns true on success
    bool constructUsageTree(Argument *arg);

    bool isTriviallyExposed(Instruction& I, int OpNo);

    bool aliasesExposed(Value *V, std::set<Value *> &exposed);

    bool transitExposedProperty(Instruction& I, MemoryLocation &Loc, int OpNo, AccessInfo isRead,
                                AccessInfo isWrite, std::set<Value *>& exposed);

    std::set<Value *> getExposedMemLocs(Function *F);

    void runOnFunction(Function *F);

    bool runOnModule(Module &M) override;

    void getAnalysisUsage(AnalysisUsage &AU) const override;

    void print(raw_ostream &OS, const Module *M) const override;

    void releaseMemory() override {
      if (mDepInfo) {
        for (auto pair : *mDepInfo)
          delete (pair.second);
        delete (mDepInfo);
      }
    };

    /// contains indexes of arguments which may be preserved by the function
    tsar::PreservedParametersInfo mParameterAccesses;
  private:
    /// contains instructions which other instructions may be dependent on
    DependentInfo *mDepInfo = nullptr;
  };

  using AddressAccessAnalyserWrapper =
  AnalysisWrapperPass<tsar::PreservedParametersInfo>;
}

#endif //SAPFOR_ADDRESSACCESS_H
