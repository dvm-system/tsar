//===--- GlobalOptions.h - Global Command Line Options ----------*- C++ -*-===//
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
// This file defines classes to store command line options.
//
//===----------------------------------------------------------------------===//

#include "tsar/Support/GlobalOptions.h"

using namespace llvm;
using namespace tsar;

char GlobalOptionsImmutableWrapper::ID = 0;
INITIALIZE_PASS(GlobalOptionsImmutableWrapper, "global-options",
  "Global Command Line Options Accessor", true, true)

ImmutablePass * llvm::createGlobalOptionsImmutableWrapper(
    const GlobalOptions *Options) {
  return new GlobalOptionsImmutableWrapper(Options);
}
