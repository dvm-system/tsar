#ifndef INSTRUMENTATION_H
#define INSTRUMENTATION_H

#include <llvm/IR/InstVisitor.h>
#include "Intrinsics.h"

class Instrumentation :public llvm::InstVisitor<Instrumentation> {
private:
  //visitCallInst and visiInvokeInst have completely the same code
  //so template for them	
  template<class T>
  void FunctionCallInst(T &I) {
    //not llvm function	  
    if(I.getCalledFunction()->isIntrinsic())
      return;	  
    //not tsar instrumentation function
    //??? maybe it's better to put some check like this in Intrinsic.h ??? 
    for(int i = 1; i<static_cast<int>(tsar::IntrinsicId::num_intrinsics); ++i){
      if(tsar::getName(static_cast<tsar::IntrinsicId>(i))
        .equals(I.getCalledFunction()->getName())) {
        return;
      }      
    }
    auto Fun = getDeclaration(I.getModule(),tsar::IntrinsicId::func_call_begin);
    llvm::CallInst::Create(Fun, {}, "", &I);
    Fun = getDeclaration(I.getModule(), tsar::IntrinsicId::func_call_end);
    auto Call = llvm::CallInst::Create(Fun, {}, "");
    Call->insertAfter(&I);
  }	  
public:	
  void visitLoadInst(llvm::LoadInst &I);
  void visitStoreInst(llvm::StoreInst &I);
  void visitCallInst(llvm::CallInst &I);
  void visitInvokeInst(llvm::InvokeInst &I);
  void visitReturnInst(llvm::ReturnInst &I);
  void visitFunction(llvm::Function &F);
};

#endif // INSTRUMENTATION_H
