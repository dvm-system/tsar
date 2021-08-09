//===- APCContext.h - Class for managing parallelization state  -*- C++ -*-===//
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
// This file declares APCContext, a container of a state of automated
// parallelization process in SAPFOR.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_APC_CONTEXT_H
#define TSAR_APC_CONTEXT_H

#include "tsar/Support/AnalysisWrapperPass.h"
#include "tsar/Support/Tags.h"
#include <apc/apc-config.h>
#include <bcl/utility.h>

struct FuncInfo;
struct FuncParam;
struct LoopGraph;
struct ParallelRegion;
struct ArrayInfo;
struct ArrayOp;
class Expression;
struct DataDirective;
struct AlignRule;
class Statement;
class Symbol;

namespace Distribution {
class Array;
class ArrayAccessInfo;
}

namespace apc {
using Expression = ::Expression;
using FuncInfo = ::FuncInfo;
using FuncParam = ::FuncParam;
using LoopGraph = ::LoopGraph;
using ParallelRegion = ::ParallelRegion;
using Statement = ::Statement;
using Symbol = ::Symbol;
using Array = Distribution::Array;
using ArrayInfo = ::ArrayInfo;
using ArrayAccessInfo = Distribution::ArrayAccessInfo;
using ArrayOp = ::ArrayOp;
using DataDirective = ::DataDirective;
using AlignRule = ::AlignRule;
}

namespace llvm {
class Function;
}

namespace tsar {
struct APCContextImpl;

class APCContext final : private bcl::Uncopyable {
public:
  APCContext();
  ~APCContext();

  /// Initialize context.
  ///
  /// We perform separate initialization to prevent memory leak. Constructor
  /// performs allocation of context storage only.
  void initialize();

  /// Returns default region which is a whole program.
  ParallelRegion & getDefaultRegion();

  /// Add a new symbol under of APCContext control, context releases memory
  /// when it becomes unused.
  ///
  /// \pre The same symbol should not be added twice. This may lead to multiple
  /// deallocation of the same memory.
  void addSymbol(apc::Symbol *S);

  /// Add loop with a specified ID under of APCContext control.
  ///
  /// If ManageMemory is set to true then context releases memory when it
  /// becomes unused.
  /// \return `false` if loop with a specified ID already exists.
  bool addLoop(ObjectID ID, apc::LoopGraph *L, bool ManageMemory = false);

  /// Return loop with a specified ID or nullptr.
  apc::LoopGraph * findLoop(ObjectID ID);

  /// Add array with a specified ID under of APCContext control, context
  /// releases memory when it becomes unused.
  bool addArray(ObjectID ID, apc::Array * A);

  /// Return array with a specified ID or nullptr.
  apc::Array * findArray(ObjectID ID);

  /// Return number of registered arrays.
  std::size_t getNumberOfArrays() const;

  /// Add description of a specified function under of APCContext control,
  /// context releases memory when it becomes unused.
  bool addFunction(llvm::Function &F, apc::FuncInfo *FI);

  /// Return information about a specified function.
  apc::FuncInfo * findFunction(const llvm::Function &F);

  APCContextImpl * const mImpl;
#ifndef NDEBUG
  bool mIsInitialized = false;
#endif
};
}

namespace llvm {
/// Wrapper to access auto-parallelization context.
using APCContextWrapper = AnalysisWrapperPass<tsar::APCContext>;
}

#endif//TSAR_APC_CONTEXT_H
