//=== Passes.cpp - Create and Initialize Analysis Passes (Flang) *- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2022 DVM System Group
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
// This contains functions to initialize passes which are necessary for
// source-base analysis of Fortran programs.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Flang/Passes.h"

using namespace llvm;

void llvm::initializeFlangAnalysis(PassRegistry &Registry) {
  initializeFlangExprMatcherPassPass(Registry);
}
