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
#include <apc/Utils/types.h>
#include <bcl/utility.h>

struct AlignRule;
struct Directive;
struct DistrVariant;
struct FuncInfo;
struct FuncParam;
struct LoopGraph;
struct ParallelDirective;
struct ParallelRegion;
struct ArrayInfo;
struct ArrayOp;
class ArrayRefExp;
class Expression;
struct DataDirective;
struct AlignRule;
class Statement;
class Symbol;
struct Messages;

namespace Distribution {
class Array;
class ArrayAccessInfo;
template<typename vType> class Arrays;
template<typename vType, typename wType, typename attrType> class GraphCSR;
}

namespace tsar {
class ParallelItem;
namespace dvmh {
class Template;
}
}

namespace apc {
using AlignRule = ::AlignRule;
using Directive = ::Directive;
using DistrVariant = ::DistrVariant;
using Expression = ::Expression;
using FuncInfo = ::FuncInfo;
using FuncParam = ::FuncParam;
using LoopGraph = ::LoopGraph;
using ParallelDirective = ::ParallelDirective;
using ParallelRegion = ::ParallelRegion;
using REMOTE_TYPE = ::REMOTE_TYPE;
using Statement = ::Statement;
using Symbol = ::Symbol;
using ArrayRefExp = ::ArrayRefExp;
using Array = Distribution::Array;
template<typename T> using Arrays = Distribution::Arrays<T>;
using ArrayInfo = ::ArrayInfo;
using ArrayAccessInfo = Distribution::ArrayAccessInfo;
using ArrayOp = ::ArrayOp;
using DataDirective = ::DataDirective;
template <typename V, typename W, typename A>
using GraphCSR = Distribution::GraphCSR<V, W, A>;
using Messages = Messages;
}

namespace llvm {
class DILocation;
class DIVariable;
class Function;
} // namespace llvm


namespace tsar {
struct APCContextImpl;

class APCContext final : private bcl::Uncopyable {
public:
  /// Return a unique name for a specified variable which is accessed in a
  /// specified function.
  static std::string getUniqueName(const llvm::DIVariable &DIVar,
                                   const llvm::Function &F);

  APCContext();
  ~APCContext();

  /// Initialize context.
  ///
  /// We perform separate initialization to prevent memory leak. Constructor
  /// performs allocation of context storage only.
  void initialize();

  /// Returns default region which is a whole program.
  ParallelRegion & getDefaultRegion();

  /// Add a new expression under of APCContext control, context releases memory
  /// when it becomes unused.
  ///
  /// \pre The same expression should not be added twice. This may lead to
  /// multiple deallocation of the same memory.
  void addExpression(apc::Expression *E);

  /// Add a new symbol under of APCContext control, context releases memory
  /// when it becomes unused.
  ///
  /// \pre The same symbol should not be added twice. This may lead to multiple
  /// deallocation of the same memory.
  void addSymbol(apc::Symbol *S);

  /// Add a new statement under of APCContext control, context releases memory
  /// when it becomes unused.
  ///
  /// \pre The same statement should not be added twice. This may lead to
  /// multiple deallocation of the same memory.
  void addStatement(apc::Statement *S);

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

  /// Add array with a specified ID under of APCContext control, context
  /// releases memory when it becomes unused.
  bool addArray(dvmh::Template *ID, apc::Array *A);

  /// Return array with a specified ID or nullptr.
  apc::Array * findArray(ObjectID ID);

  /// Return array with a specified ID or nullptr.
  apc::Array * findArray(dvmh::Template *ID);

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
