#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

#include <llvm/ADT/BitmaskEnum.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/InstVisitor.h>
#include <llvm/Analysis/LoopInfo.h>
#include "Intrinsics.h"
#include "ItemRegister.h"
#include "tsar_instrumentation.h"
#include "CanonicalLoop.h"
#include "DFRegionInfo.h"
#include <sstream>
#include <iostream>

namespace llvm {
class AllocaInst;
class DebugLoc;
class DILocation;
class DIVariable;
class GlobalVariable;
class Function;
class Loop;
class Type;
class Value;
class SCEV;

class LoopInfo;
class DominatorTree;
class ScalarEvolution;
}

namespace tsar {
class DFRegionInfo;

LLVM_ENABLE_BITMASK_ENUMS_IN_NAMESPACE();

class Instrumentation : public llvm::InstVisitor<Instrumentation> {
  using Base = llvm::InstVisitor<Instrumentation>;
  using TypeRegister = ItemRegister<llvm::Type *>;
  using DIStringRegister = ItemRegister<
    llvm::AllocaInst *, llvm::GlobalVariable *, llvm::Instruction *,
    llvm::Function *, llvm::Loop *, llvm::DILocation *, llvm::Value *>;

  enum LoopBoundKind : short {
    LoopBoundIsUnknown = 0,
    LoopStartIsKnown = 1u << 0,
    LoopEndIsKnown = 1u << 1,
    LoopStepIsKnown = 1u << 2,
    LoopBoundUnsigned = 1u << 3,
    LLVM_MARK_AS_BITMASK_ENUM(LoopBoundUnsigned)
  };
public:
  static const unsigned maxIntBitWidth = 64;

  Instrumentation(llvm::Module& M, llvm::InstrumentationPass* I);
  ~Instrumentation() = default;

  using Base::visit;

  /// Visit function if it should be processed only.
  void visit(llvm::Function &F);

  void visitAllocaInst(llvm::AllocaInst &I);
  void visitLoadInst(llvm::LoadInst &I);
  void visitStoreInst(llvm::StoreInst &I);
  void visitReturnInst(llvm::ReturnInst &I);
  void visitFunction(llvm::Function &F);
  void visitCallSite(llvm::CallSite CS);

private:

  void instrumentateMain(llvm::Module& M);

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
    llvm::DISubprogram *MD, DIStringRegister::IdTy Idx, llvm::Module &M);

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
  /// \param [in] V IR-level description of the variable.
  /// \param [in] T Type of the variable. Note, the this must be a type of a
  /// variable, not a pointer to the variable. In case of `alloca` this type
  /// is 'allocated type' and in case of global variable this type is
  /// 'value type'.
  /// \param [in] MD Debug information for the registered variable. It may be
  /// `nullptr`. In this case some information for the variable will not
  /// be available after instrumentation.
  /// \param [in] Idx Index of a metadata string in the pool of strings.
  /// \param [in] InsertBefore This instruction identify position to insert
  /// necessary instructions.
  /// \param [in] M A module which is processed.
  void regValue(llvm::Value *V, llvm::Type *T, llvm::DIVariable *MD,
    DIStringRegister::IdTy Idx, llvm::Instruction &InsertBefore,
    llvm::Module &M);

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

  /// Registers a metadata string for a specified location. If this location is
  /// nullptr, than previously reserved string is used. The function returns
  /// index of the metadata string.
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
  llvm::Function *mInitDIAll = nullptr;
};
}

#endif // INSTRUMENTATION_H
