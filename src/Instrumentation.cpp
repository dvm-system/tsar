#include <llvm/IR/InstIterator.h>
#include "Instrumentation.h"
#include "Intrinsics.h"

using namespace llvm;
using namespace tsar;

void Instrumentation::visitLoadInst(llvm::LoadInst &I) {
  Function* Fun;	
  //looking for operand type
  auto Operand = static_cast<GetElementPtrInst*>(I.getPointerOperand())
    ->getPointerOperand();
  auto TypeID = Operand->getType()->getTypeID();  
  //depending on operand type insert a sapforReadVar or sapforReadArr
  //consider all pointers as arrays
  if(TypeID != Type::TypeID::PointerTyID && TypeID != Type::TypeID::ArrayTyID) {
    Fun = getDeclaration(I.getModule(), IntrinsicId::read_var);	
  } else {
    Fun = getDeclaration(I.getModule(), IntrinsicId::read_arr);	
  }
  CallInst::Create(Fun, {}, "", &I);
}

void Instrumentation::visitStoreInst(llvm::StoreInst &I) {
  Function* Fun;	
  //looking for operand type
  auto Operand = static_cast<GetElementPtrInst*>(I.getPointerOperand())
    ->getPointerOperand();
  auto TypeID = Operand->getType()->getTypeID();  
  //depending on operand type insert a sapforWriteVarEnd or sapforWriteArrEnd
  //consider all pointers as arrays
  if(TypeID != Type::TypeID::PointerTyID && TypeID != Type::TypeID::ArrayTyID) {
    Fun = getDeclaration(I.getModule(), IntrinsicId::write_var_end);	
  } else {
    Fun = getDeclaration(I.getModule(), IntrinsicId::write_arr_end);	
  }
  auto Call = CallInst::Create(Fun, {}, "");
  Call->insertAfter(&I);
}

void Instrumentation::visitCallInst(llvm::CallInst &I) {
  //using template method
  FunctionCallInst(I);	
}

void Instrumentation::visitInvokeInst(llvm::InvokeInst &I) {
  //using template method
  FunctionCallInst(I);	
}

void Instrumentation::visitReturnInst(llvm::ReturnInst &I) {
  //not llvm function	
  if(I.getFunction()->isIntrinsic())
    return;	  
  //not tsar instrumentation function
  for(int i = 1; i<static_cast<int>(tsar::IntrinsicId::num_intrinsics); ++i){
    if(tsar::getName(static_cast<tsar::IntrinsicId>(i))
      .equals(I.getFunction()->getName())) {
      return;
    }      
  }
  auto Fun = getDeclaration(I.getModule(), IntrinsicId::func_end);
  CallInst::Create(Fun, {}, "", &I);	  
}

void Instrumentation::visitFunction(llvm::Function &F) {
  if(F.isIntrinsic())
    return;	  
  for(int i = 1; i < static_cast<int>(IntrinsicId::num_intrinsics); ++i) {
    if(getName(static_cast<IntrinsicId>(i)).equals(F.getName()))
      return;	    
  }
  auto Fun = getDeclaration(F.getParent(), IntrinsicId::func_begin);
  auto Begin = inst_begin(F);	  
  CallInst::Create(Fun, {}, "", &(*Begin));
}


