//===-------- main.cpp ------ Traits Static Analyzer ------------*- C++ -*-===//
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
// Traits Static Analyzer (TSAR) is a part of a system for automated
// parallelization SAPFOR. This file declares interfaces to execute analysis
// and to perform transformations.
//
//===----------------------------------------------------------------------===//

#include "tsar/Core/Tool.h"
#include <llvm/Support/Debug.h>
#include <llvm/Support/ManagedStatic.h>
#include <llvm/Support/PrettyStackTrace.h>
#include <llvm/Support/Signals.h>

using namespace llvm;
using namespace tsar;

int main(int Argc, const char** Argv) {
  sys::PrintStackTraceOnErrorSignal(Argv[0]);
  PrettyStackTraceProgram StackTraceProgram(Argc, Argv);
  EnableDebugBuffering = true;
  llvm_shutdown_obj ShutdownObj; //call llvm_shutdown() on exit
  Tool Analyzer(Argc, Argv);
  return Analyzer.run();
}