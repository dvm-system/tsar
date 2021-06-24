//===- GlobalsAccess.h --- Globals Access Collector -------------*- C++ -*-===//
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
// This file implements a pass to collect explicit accesses to global values in
// a function.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_GLOBALS_ACCESS_H
#define TSAR_GLOBALS_ACCESS_H

#include "tsar/Support/AnalysisWrapperPass.h"
#include <llvm/IR/ValueHandle.h>
#include <llvm/IR/ValueMap.h>

namespace tsar {
using GlobalsAccessMap =
    llvm::ValueMap<llvm::Function *,
                   llvm::SmallVector<llvm::WeakTrackingVH, 1>>;
}

namespace llvm {
/// Wrapper to access global variables referenced in functions.
using GlobalsAccessWrapper =  AnalysisWrapperPass<tsar::GlobalsAccessMap>;
}
#endif//TSAR_GLOBALS_ACCESS_H
