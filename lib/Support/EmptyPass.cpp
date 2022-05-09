//===---- EmptyPass.cpp ------ Trivial Empty Pass ----------------*- C++ -*===//
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
// This file implements a trivial empty pass which does nothing. It can be use
// as a stub if configuration of the build does not provide some passes.
//
//===----------------------------------------------------------------------===//

#include "tsar/Support/EmptyPass.h"

using namespace llvm;

char EmptyFunctionPass::ID = 0;
INITIALIZE_PASS(EmptyFunctionPass, "empty-function-pass", "Empty Function Pass",
                true, true)

char EmptyModulePass::ID = 0;
INITIALIZE_PASS(EmptyModulePass, "empty-module-pass", "Empty Module Pass", true,
                true)
