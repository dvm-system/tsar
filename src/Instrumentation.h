#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

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
class GlobalVariable;
class Function;
class Loop;
class DILocation;
}

namespace tsar {
class Instrumentation :public llvm::InstVisitor<Instrumentation> {
  using TypeRegister = ItemRegister<llvm::Type *>;
  using CtxStringRegister = ItemRegister<
    llvm::AllocaInst *, llvm::GlobalVariable *, llvm::Instruction *,
    llvm::Function *, llvm::Loop *, llvm::DILocation *>;
public:
  static const unsigned maxIntBitWidth = 64;

  Instrumentation(llvm::Module& M, llvm::InstrumentationPass* const I);
  ~Instrumentation() = default;

  void visitAllocaInst(llvm::AllocaInst &I);
  void visitLoadInst(llvm::LoadInst &I);
  void visitStoreInst(llvm::StoreInst &I);
  void visitCallInst(llvm::CallInst &I);
  void visitInvokeInst(llvm::InvokeInst &I);
  void visitReturnInst(llvm::ReturnInst &I);
  void visitFunction(llvm::Function &F);

private:
  llvm::LoopInfo* mLoopInfo = nullptr;
  llvm::DFRegionInfo* mRegionInfo = nullptr;
  tsar::CanonicalLoopSet* mCanonicalLoop = nullptr;
  llvm::InstrumentationPass* const mInstrPass;

  //visitCallInst and visiInvokeInst have completely the same code
  //so template for them
  //
  //NOTE: Instead of template it was possible to overload visitCallSite which 
  //is for both calls and invokes. Maybe i'll change it later. 
  template<class T>
  void FunctionCallInst(T &I) {
    //not llvm function
    auto Callee =
      llvm::dyn_cast<llvm::Function>(I.getCalledValue()->stripPointerCasts());
    // TODO (kaniandr@gmail.com): print warrning in case of Callee == nullptr.
    if(!Callee || Callee->isIntrinsic())
      return;
    //not tsar function
    tsar::IntrinsicId Id;
    if(getTsarLibFunc(Callee->getName(), Id)) {
      return;
    }
    std::stringstream Debug;
    Debug << "type=func_call*file=" << I.getModule()->getSourceFileName()
      << "*line1=" << I.getDebugLoc()->getLine() << "*name1=" << 
      Callee->getName().str() << "*rank=" <<
      Callee->getFunctionType()->getNumParams() << "**";
    auto CallIdx = mCtxStrings.regItem<llvm::Instruction *>(&I);
    regDbgStr(Debug.str(), *I.getModule(), CallIdx);
    auto DICall = getDbgPoolElem(CallIdx, I);
    auto Fun = getDeclaration(I.getModule(),tsar::IntrinsicId::func_call_begin);
    llvm::CallInst::Create(Fun, {DICall}, "", &I);
    Fun = getDeclaration(I.getModule(), tsar::IntrinsicId::func_call_end);
    auto Call = llvm::CallInst::Create(Fun, {DICall}, "");
    Call->insertAfter(&I);
  }

  //visitBasicBlock depends on LoopInfo which is different for
  //different Functions. Calling it from wherever except visitFunction
  //could lead to using unappropriate LoopInfo. So moved it to private.
  void visitBasicBlock(llvm::BasicBlock &B);

  void loopBeginInstr(llvm::Loop *L, llvm::BasicBlock& Header, unsigned);
  void loopEndInstr(llvm::Loop const *L, llvm::BasicBlock& Header, unsigned);
  void loopIterInstr(llvm::Loop *L, llvm::BasicBlock& Header, unsigned);

  unsigned regDbgStr(const std::string& S, llvm::Module& M);
  void regTypes(llvm::Module& M);
  llvm::GetElementPtrInst* prepareStrParam(const std::string& S, 
    llvm::Instruction &I);
  llvm::LoadInst* getDbgPoolElem(
    CtxStringRegister::IdTy Val, llvm::Instruction& I);
  void regGlobals(llvm::Module& M);
  void instrumentateMain(llvm::Module& M); 

  TypeRegister mTypes;
  CtxStringRegister mCtxStrings;
};

#endif // INSTRUMENTATION_H
