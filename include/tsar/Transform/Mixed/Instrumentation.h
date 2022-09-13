//===--- Instrumentation.h - LLVM IR Instrumentation Engine -----*- C++ -*-===//
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
// This file define functionality to perform IR-level instrumentation.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_INSTRUMENTATION_H
#define TSAR_INSTRUMENTATION_H

#include "tsar/ADT/ItemRegister.h"
#include "tsar/Analysis/Clang/CanonicalLoop.h"
#include "tsar/Transform/Mixed/Passes.h"
#include <bcl/utility.h>
#include <llvm/ADT/ArrayRef.h>
#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/Optional.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Pass.h>

namespace llvm {
class DominatorTree;
class Loop;
class LoopInfo;
class ScalarEvolution;

/// This per-module pass performs instrumentation of LLVM IR.
class InstrumentationPass :
  public ModulePass, bcl::Uncopyable {
public:
  /// Pass identification, replacement for typeid.
  static char ID;

  /// Default constructor.
  InstrumentationPass() : ModulePass(ID) {
    initializeInstrumentationPassPass(*PassRegistry::getPassRegistry());
  }

  /// This construction specifies a function
  /// where should be placed initialization of metadata.
  ///
  /// If `StartFrom` is not empty all mentioned functions and transitive
  /// callees from these functions should be processed only.
  /// Other functions will be marked with sapfor.da.ignore metadata.
  InstrumentationPass(StringRef InstrEntry, ArrayRef<std::string> StartFrom) :
      ModulePass(ID), mInstrEntry(InstrEntry),
      mStartFrom(StartFrom.begin(), StartFrom.end()) {
    initializeInstrumentationPassPass(*PassRegistry::getPassRegistry());
  }

  /// Implements the per-module instrumentation pass.
  bool runOnModule(Module &M) override;

  /// Set analysis information that is necessary to run this pass.
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  /// Return name of a function which contains metadata initialization.
  StringRef getEntryName() const { return mInstrEntry;}

  /// Return names of functions where instrumentation is started.
  ArrayRef<std::string> getStartFrom() const { return mStartFrom; }

private:
  std::string mInstrEntry;
  std::vector<std::string> mStartFrom;
};
}

namespace tsar {
class DFRegionInfo;
struct DIMemoryLocation;

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

/// \brief Creates an empty "sapfor.init.di" function instruction to initialize
/// all metadata strings which is necessary for instrumentation.
///
/// The entry block of this function will contain only `ret` instruction.
/// This function will be marked with 'sapfor.da' metadata. This metadata
/// will be also inserted into the named 'sapfor.da' metadata of a specified
/// module.
llvm::Function * createEmptyInitDI(llvm::Module &M, llvm::Type &IdTy);

/// \brief Return external variable which refers to a "sapfor.di.pool" in
/// a specified module and a type of a pool element.
///
/// \return This function returns 'nullptr', if a global value with the
/// 'sapfor.di.pool' name already exists and it can not be used as a pool.
std::pair<llvm::GlobalVariable *, llvm::Type *>
getOrCreateDIPool(llvm::Module &M);

/// \brief Processes a specified entry point.
///
/// This function performs instrumentation of a specified entry point.
/// This functions allocates a pool to store metadata strings of each
/// specified module and calls functions to initialize metadata strings,
/// types, global objects for each module.
/// \pre
/// - An external pool must be declared in each module (see getOrCreateDIPool).
/// - Named metadata for each module must contain description of objects
/// that should be initialized (see addNamedDAMetadata).
void visitEntryPoint(llvm::Function &Entry,
  llvm::ArrayRef<llvm::Module *> Modules);

/// Returns type of identifier which is used while instrumentation is performed.
llvm::Type *getInstrIdType(llvm::LLVMContext &Ctx);

class Instrumentation : public llvm::InstVisitor<Instrumentation> {
  using Base = llvm::InstVisitor<Instrumentation>;
  using TypeRegister = ItemRegister<llvm::Type *>;
  /// \breif This pair allows us to distinguish loops from different functions.
  ///
  /// Only one loop tree is available at a time. So, it is possible that number
  /// of addresses of loops from one tree overlaps with addresses of loops in
  /// another tree (at this moment the first tree has been already destroyed and
  /// the second one has been already created). Hence, we use a pair to
  /// accurately identify loops.
  using LoopUnique = std::pair<llvm::Function *, llvm::Loop *>;
  using DIStringRegister = ItemRegister<
    llvm::AllocaInst *, llvm::GlobalVariable *, llvm::Instruction *,
    llvm::Function *, LoopUnique, llvm::DILocation *, llvm::Value *>;

  enum LoopBoundKind : short {
    LoopBoundIsUnknown = 0,
    LoopStartIsKnown = 1u << 0,
    LoopEndIsKnown = 1u << 1,
    LoopStepIsKnown = 1u << 2,
    LoopBoundUnsigned = 1u << 3,
    LLVM_MARK_AS_BITMASK_ENUM(LoopBoundUnsigned)
  };
public:
  /// Processes a specified module.
  static void visit(llvm::Module &M, llvm::InstrumentationPass &IP) {
    Instrumentation Visitor;
    Visitor.visitModule(M, IP);
  }

  using Base::visit;

  /// Visit function if it should be processed only.
  void visit(llvm::Function &F);

  void visitModule(llvm::Module &M, llvm::InstrumentationPass &IP);
  void visitInstruction(llvm::Instruction &I);
  void visitAllocaInst(llvm::AllocaInst &I);
  void visitLoadInst(llvm::LoadInst &I);
  void visitStoreInst(llvm::StoreInst &I);
  void visitAtomicCmpXchgInst(llvm::AtomicCmpXchgInst &I);
  void visitAtomicRMWInst(llvm::AtomicRMWInst &I);
  void visitReturnInst(llvm::ReturnInst &I);
  void visitFunction(llvm::Function &F);
  void visitCallBase(llvm::CallBase &Call);

private:
  /// Mark functions which should be ignored with sapfor.da.ignore metadata.
  ///
  /// This method does not mark functions which are always ingored,
  /// for example, functions which are marked as 'sapfor.da'.
  /// \post If -instr-start option is specified all functions except
  /// mentioned functions and transitive callees from these functions
  /// will be marked with 'sapfor.da.ignore'.
  void excludeFunctions(llvm::Module &M);

  void regReadMemory(llvm::Instruction &I, llvm::Value &Ptr);
  void regWriteMemory(llvm::Instruction &I, llvm::Value &Ptr);

  /// Reserves some metadata string for object which have not enough
  /// information.
  void reserveIncompleteDIStrings(llvm::Module &M);

  /// \brief Registers metadata string for a specified function `F`.
  ///
  /// Metadata parameter `MD` is optional. If it is `nullptr` information
  /// which is available from LLVM IR will be only used to construct a
  /// metadata string.
  ///
  /// A specified value `F` may not be a function, for example if a pointer is
  /// used in a call instruction.
  void regFunction(llvm::Value &F, llvm::Type *ReturnTy, unsigned Rank,
    llvm::DINode *MD, DIStringRegister::IdTy Idx, llvm::Module &M);

  /// Registers arguments of a specified function which have not been promoted
  /// to registers.
  void regArgs(llvm::Function &F, llvm::LoadInst *DIFunc);

  /// \brief Returns parameter for sapforRegVar(...) or sapforRegArr(...)
  /// functions.
  ///
  /// \return
  /// - metadata string for debug location,
  /// - address of accessed memory,
  /// - metadata string for accessed memory,
  /// - address of array base (in case of array access) or nullptr.
  std::tuple<llvm::Value *, llvm::Value *, llvm::Value *, llvm::Value *>
    regMemoryAccessArgs(llvm::Value *Ptr, const llvm::DebugLoc &DbgLoc,
      llvm::Instruction &InsertBefore);

  /// \brief Registers a metadata string and a variable.
  ///
  /// \post Insert calls of `sapforInitDI` and `sapforRegVar/sapforRegArr`.
  void regValue(llvm::Value *V, llvm::Type *T, llvm::Value *ArraySize,
    const DIMemoryLocation *DIM, DIStringRegister::IdTy Idx,
    llvm::Instruction &InsertBefore, llvm::Module &M);

  /// Prepare arguments to register a specified value.
  ///
  /// \param [in] V IR-level description of the variable.
  /// \param [in] T Type of the variable. Note, the this must be a type of a
  /// variable, not a pointer to the variable. In case of `alloca` this type
  /// is 'allocated type' and in case of global variable this type is
  /// 'value type'.
  /// \param [in] ArraySize Number of allocated elements of a type T and
  /// start adress V.
  /// \param [in] SizeArgTy Type of ArraySize parameter in registration
  /// intrinsic, for example in `sapforRegArr`.
  /// \param [in] DIM Debug information for the registered memory location.
  /// It may be `nullptr`. In this case some information for the variable will
  /// not be available after instrumentation.
  /// \param [in] Idx Index of a metadata string in the pool of strings.
  /// \param [in] InsertBefore This instruction identify position to insert
  /// necessary instructions.
  /// \param [in] M A module which is processed.
  /// \param [out] Args General arguments which is necessary to register
  /// scalar/array variable.
  void regValueArgs(llvm::Value *V, llvm::Type *T,
    llvm::Value *ArraySize, llvm::Type *SizeArgTy,
    const DIMemoryLocation *DIM, DIStringRegister::IdTy Idx,
    llvm::Instruction &InsertBefore, llvm::Module &M,
    llvm::SmallVectorImpl<llvm::Value *> &Args);

  /// Registers a metadata string for each declared function (except functions
  /// which are marked with 'sapfor.da' metadata).
  void regFunctions(llvm::Module &M);

  /// \brief Registers global variables.
  ///
  /// This function registers a metadata string for each global variables.
  /// A separate function to register all globals (call of sapforRegVar())
  /// will be also created.
  void regGlobals(llvm::Module& M);

  /// Registers types which are used in a specified module.
  void regTypes(llvm::Module& M);

  /// Registers a metadata string for a specified location if it has not been
  /// registered yet. If this location is nullptr, than previously reserved
  /// string is used. The function returns index of the metadata string.
  DIStringRegister::IdTy regDebugLoc(const llvm::DebugLoc &DbgLoc);

  /// \brief Inserts a call of sapforInitDI(...) and registers a specified
  /// metadata string.
  ///
  /// \param [in] Str Metadata string that should be registered.
  /// \param [in] Idx Index of metadata which corresponds to the string
  /// in the pool.
  /// \param [in,out] M Module which is being processed.
  void createInitDICall(const llvm::Twine &Str, DIStringRegister::IdTy Idx);

  /// \brief Creates a global array of characters and returns GEP to access
  /// this array.
  ///
  /// Global metadata string will be marked with "sapfor.da" metadata.
  llvm::GetElementPtrInst * createDIStringPtr(llvm::StringRef Str,
    llvm::Instruction &InsertBefore);

  /// \brief Returns description of metadata with a specified index in the pool.
  ///
  /// All inserted instructions which access memory will be stored in
  /// the mIgnoreMemoryAccess set.
  llvm::LoadInst* createPointerToDI(
    DIStringRegister::IdTy Idx, llvm::Instruction &InsertBefore);

  /// \brief Creates instructions to compute a specified SCEV if possible.
  ///
  /// \pref DominatorTree (mDT) and ScalarEvoultion (mSE) must not be null.
  /// \post
  /// - Created instructions will be safely inserted before `InsertBefore`.
  /// - The result should have a specified integer type `IntTy`.
  /// - The result should be signed if `Signed` is `true`.
  /// - All new instructions will be marked with 'sapfor.da' metadata.
  llvm::Value * computeSCEV(const llvm::SCEV *ExprSCEV,
    llvm::IntegerType &IntTy, bool Signed,
    llvm::ScalarEvolution &SE, llvm::DominatorTree &DT,
    llvm::Instruction &InsertBefore);

  /// Registers all loops in a specified function
  void regLoops(llvm::Function &F, llvm::LoopInfo &LI,
    llvm::ScalarEvolution &SE, llvm::DominatorTree &DT,
    DFRegionInfo &RI, const CanonicalLoopSet &CS);

  /// Registers metadata string which describes a loop and inserts call of
  /// sapforSLBegin() function.
  void loopBeginInstr(llvm::Loop *L, DIStringRegister::IdTy DILoopIdx,
    llvm::ScalarEvolution &SE, llvm::DominatorTree &DT,
    DFRegionInfo &RI, const CanonicalLoopSet &CS);

  /// This function creates a new basic block between exiting and exit blocks
  /// and inserts call of sapforSLEnd() in this new block.
  void loopEndInstr(llvm::Loop *L, DIStringRegister::IdTy DILoopIdx);

  /// \brief Add counter to determine number of current loop iteration.
  ///
  /// A start value of the counter is 1. The counter is an argument for
  /// sapforSIter() function. Note, that this counter has not been presented in
  /// a source code.
  void loopIterInstr(llvm::Loop *L, DIStringRegister::IdTy DILoopIdx);

  /// \brief Creates instructions to compute bounds and step of canonical loop.
  ///
  /// \return <start,end,step,signed> tuple, if some of values can not be
  /// computed (for example a loop is not in canonical form) return `nullptr`.
  /// \post Instructions to compute values will be inserted before terminator
  /// of a loop preheader.
  std::tuple<llvm::Value *, llvm::Value *, llvm::Value *, bool>
    computeLoopBounds(llvm::Loop &L, llvm::IntegerType &IntTy,
      llvm::ScalarEvolution &SE, llvm::DominatorTree &DT,
      DFRegionInfo &RI, const CanonicalLoopSet &CS);

  /// Recursively delete instruction with empty list of uses (for all deleted
  /// instructions a parent must be specified).
  void deleteDeadInstructions(llvm::Instruction *From);

  /// Recursively set "sapfor.da" metadata for a specified instruction `From`
  /// and its operands, if `Form` has no uses (while each operand has a single
  /// use).
  void setMDForDeadInstructions(llvm::Instruction *From);

  /// Recursively set "sapfor.da" metadata for a specified instruction 'From'
  /// and its operands, it each of 'Form' and the operands has single use.
  void setMDForSingleUseInstructions(llvm::Instruction *From);

  llvm::InstrumentationPass *mInstrPass = nullptr;
  TypeRegister mTypes;
  DIStringRegister mDIStrings;
  llvm::GlobalVariable *mDIPool = nullptr;
  llvm::Type *mDIPoolElementTy = nullptr;
  llvm::Function *mInitDIAll = nullptr;
  /// Dominator tree of a currently processed function.
  llvm::DominatorTree *mDT = nullptr;
};
}

#endif//TSAR_INSTRUMENTATION_H
