//===--- ServerUtils.h ------ Analysis Server Utils -------------*- C++ -*-===//
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
//===----------------------------------------------------------------------===//
//
// This file defines functions to simplify client to server data mapping.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ANALYSIS_MEMORY_SERVER_H
#define TSAR_ANALYSIS_MEMORY_SERVER_H

#include <llvm/Transforms/Utils/ValueMapper.h>

namespace llvm {
class Module;
class AnalysisUsage;
class Pass;
namespace legacy {
class PassManager;
}
}

namespace tsar {
/// Enable mapping of metadata-level memory locations attached to a
/// metadata-level alias tree.
struct ClientToServerMemory {
  static void prepareToClone(llvm::Module &ClientM,
    llvm::ValueToValueMapTy &ClientToServer);

  static void initializeServer(llvm::Pass &P, llvm::Module &ClientM,
    llvm::Module &ServerM, llvm::ValueToValueMapTy &ClientToServer,
    llvm::legacy::PassManager &PM);

  static void prepareToClose(llvm::legacy::PassManager &PM);

  static void getAnalysisUsage(llvm::AnalysisUsage &AU);
};
}
#endif//TSAR_ANALYSIS_MEMORY_SERVER_H
