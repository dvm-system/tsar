//===--- PassBarrier.h ---------- Pass Barrier ------------------*- C++ -*-===//
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
//===----------------------------------------------------------------------===//
//
// Some function passes (for example DefinedMemoryPass) may use results of
// module passes and these results becomes invalid after transformations.
// A pass in this file do nothing but allow us to finish processing of
// all functions before transformations.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_PASS_BARRIER_H
#define TSAR_PASS_BARRIER_H

namespace llvm {
class ModulePass;
class PassRegistry;

void initializePassBarrierPass(PassRegistry &Registry);

ModulePass *createPassBarrier();
}

#endif//TSAR_PASS_BARRIER_H
