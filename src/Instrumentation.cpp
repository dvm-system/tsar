#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include "tsar_utility.h"
#include "Instrumentation.h"
#include "Intrinsics.h"
#include <iostream>

using namespace llvm;
using namespace tsar;

Instrumentation::Instrumentation(Module &M, InstrumentationPass* const I)
  : mInstrPass(I), mLoopInfo(*(new LoopInfo())) {
  const std::string& ModuleName = M.getModuleIdentifier();
  //create function for types registration
  const std::string& RegTypeFuncName = "RegType" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto RegType = FunctionType::get(Type::getVoidTy(M.getContext()), 
    {Type::getInt32Ty(M.getContext())}, false);
  auto RegFunc = Function::Create(RegType, 
    GlobalValue::LinkageTypes::InternalLinkage, RegTypeFuncName, &M);
  auto NewBlock = BasicBlock::Create(RegFunc->getContext(), "entry", RegFunc);
  ReturnInst::Create(RegFunc->getContext(), NewBlock);
  //insert declaration of pool for debug information
  const std::string& DbgPoolName = "gSapforDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto DbgPool = new GlobalVariable(M, PointerType::getUnqual(Type
    ::getInt8PtrTy(M.getContext())), false, GlobalValue::LinkageTypes
    ::ExternalLinkage, nullptr, DbgPoolName, nullptr);
  DbgPool->setAlignment(4);
  //create function for debug information initialization
  const std::string& InitFuncName = "sapforInitDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto Type = FunctionType::get(Type::getVoidTy(M.getContext()), false);
  auto Func = Function::Create(Type, GlobalValue::LinkageTypes::InternalLinkage,
    InitFuncName, &M);
  NewBlock = BasicBlock::Create(Func->getContext(), "entry", Func);
  ReturnInst::Create(Func->getContext(), NewBlock);
  regGlobals(M);
  //visit all functions
  for(auto& F: M) {
    //not sapforInitDI function
    if(F.getSubprogram() != nullptr) {
      visitFunction(F);
    }
  }
  //insert call to allocate debug information pool
  auto Fun = getDeclaration(&M, IntrinsicId::allocate_pool);
  //getting numb of registrated debug strings by the next value returned from
  //registrator. don't go through function to count inserted calls.
  auto Idx = ConstantInt::get(Type::getInt64Ty(M.getContext()), 
    mRegistrator.getDbgStrCounter());
  CallInst::Create(Fun, {DbgPool, Idx}, "", &(*inst_begin(Func)));
}

//insert call of sapforRegVar(void*, void*) or 
//sapforRegArr(void*, size_t, void*) after specified alloca instruction.
void Instrumentation::visitAllocaInst(llvm::AllocaInst &I) {
  //FIXME: need to add functions parameters registration. instrumentation fails
  // without that. 
  auto Metadata = getMetadata(&I);
  if(Metadata == nullptr) 
    return;
  unsigned ID = getTypeId(*I.getAllocatedType());
  std::stringstream Debug;
  auto Addr = new BitCastInst(&I, Type::getInt8PtrTy(I.getContext()), "Addr");
  cast<Instruction>(Addr)->insertAfter(&I);
  auto TypeId = I.getAllocatedType()->getTypeID();
  //Alloca instruction has isArrayAllocation method, but it looks like it
  //doesn't work like i wanted it. So checking for array in this way
  if(TypeId == Type::TypeID::ArrayTyID || TypeId == Type::TypeID::PointerTyID){
    mRegistrator.regArr(Metadata->getName().data(), Metadata->getLine());
    Debug << "type=arr_name*file=" << I.getModule()->getSourceFileName() <<
      "*line1=" << Metadata->getLine() << "*name1=" << 
      Metadata->getName().data() << "*vtype=" << ID << "*rank=1**";
    auto DIVar = getDbgPoolElem(regDbgStr(Debug.str(), *I.getModule()), I);
    auto ArrSize = ConstantInt::get(Type::getInt64Ty(I.getContext()),
      I.getAllocatedType()->getArrayNumElements());
    auto Fun = getDeclaration(I.getModule(), IntrinsicId::reg_arr);
    auto Call = CallInst::Create(Fun, {DIVar, ArrSize, Addr}, "");
    Call->insertAfter(Addr);
  } else {	  
    mRegistrator.regVar(Metadata->getName().data(), Metadata->getLine());
    Debug << "type=var_name*file=" << I.getModule()->getSourceFileName() <<
      "*line1=" << Metadata->getLine() << "*name1=" << 
      Metadata->getName().data() << "*vtype=" << ID << "**";
    auto DIVar = getDbgPoolElem(regDbgStr(Debug.str(), *I.getModule()), I);
    auto Fun = getDeclaration(I.getModule(), IntrinsicId::reg_var);
    auto Call = CallInst::Create(Fun, {DIVar, Addr}, "");
    Call->insertAfter(Addr);
  }
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
  IntrinsicId Id;
  if(getTsarLibFunc(I.getFunction()->getName().data(), Id)) {
    return;
  }
  auto Fun = getDeclaration(I.getModule(), IntrinsicId::func_end);
  std::stringstream Debug;
  auto F = I.getFunction();
  Debug << "type=function*file=" << F->getParent()->getSourceFileName() <<
    "*line1=" << F->getSubprogram()->getLine() << "*line2=" <<
    (F->getSubprogram()->getLine() + F->getSubprogram()->getScopeLine()) <<
    "*name1=" << F->getSubprogram()->getName().data() << "*vtype=" <<
    getTypeId(*F->getReturnType()) << "*rank=" <<
    F->getFunctionType()->getNumParams() << "**";
  auto DIFunc = getDbgPoolElem(regDbgStr(Debug.str(), *I.getModule()), I);
  auto Call = CallInst::Create(Fun, {DIFunc}, "");
  Call->insertAfter(DIFunc);
}

void Instrumentation::loopBeginInstr(llvm::Loop const *L,
  llvm::BasicBlock &Header){
  //looking through all possible loop predeccessors
  for(auto it = pred_begin(&Header), et = pred_end(&Header); it != et; ++it) {
    BasicBlock* Predeccessor = *it;
    if(!L->contains(Predeccessor)) {
      auto ExitInstr = Predeccessor->getTerminator();
      //looking for successor which is our Header
      for(unsigned I = 0; I < ExitInstr->getNumSuccessors(); ++I) {
        //create a new BasicBlock between predeccessor and Header
        //insert a function call there
        if(ExitInstr->getSuccessor(I) == &Header) {
          //looks like label "loop_begin" would be
          //renamed automatically to make it unique
          auto Block4Insert = BasicBlock::Create(Header.getContext(),
            "loop_begin", Header.getParent());
          IRBuilder<> Builder(Block4Insert, Block4Insert->end());
          Builder.CreateBr(ExitInstr->getSuccessor(I));
          ExitInstr->setSuccessor(I, Block4Insert);
          Builder.SetInsertPoint(&(*const_cast<BasicBlock *>
            (Block4Insert)->begin()));
          auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_begin);
          auto Call = Builder.CreateCall(Fun);
        }
      }
    }
  }
}

void Instrumentation::loopEndInstr(llvm::Loop const *L,
  llvm::BasicBlock &Header) {
  //Creating a node between inside/outside blocks of the loop. Then insert
  //call of tsarSLEnd() in this node.
  SmallVector<BasicBlock*, 8> ExitBlocks;
  SmallVector<BasicBlock*, 8> ExitingBlocks;
  L->getExitBlocks(ExitBlocks);
  L->getExitingBlocks(ExitingBlocks);
  for(auto Exiting: ExitingBlocks) {
    auto ExitInstr = Exiting->getTerminator();
    for(unsigned SucNumb = 0; SucNumb<ExitInstr->getNumSuccessors(); ++SucNumb){
      for(auto Exit : ExitBlocks) {
        if(Exiting->getTerminator()->getSuccessor(SucNumb) == Exit) {
          auto Block4Insert = BasicBlock::Create(Header.getContext(),
            "loop_exit", Header.getParent());
          IRBuilder<> Builder(Block4Insert, Block4Insert->end());
          Builder.CreateBr(ExitInstr->getSuccessor(SucNumb));
          ExitInstr->setSuccessor(SucNumb, Block4Insert);
          Builder.SetInsertPoint(&(*const_cast<BasicBlock *>
            (Block4Insert)->begin()));
          auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_end);
          auto Call = Builder.CreateCall(Fun);
        }
      }
    }
  }
}

void Instrumentation::loopIterInstr(llvm::Loop const *L,
  llvm::BasicBlock &Header) {
  auto Fun = getDeclaration(Header.getModule(), IntrinsicId::sl_iter);
  CallInst::Create(Fun, {}, "", Header.getTerminator());
} 

void Instrumentation::visitBasicBlock(llvm::BasicBlock &B) {
  if(mLoopInfo.isLoopHeader(&B)) {
    auto Loop = mLoopInfo.getLoopFor(&B);
    loopBeginInstr(Loop, B);
    loopEndInstr(Loop, B);
    loopIterInstr(Loop, B);
  }
  //visit all Instructions
  for(auto& I : B){
    visit(I);
  }
}

void Instrumentation::visitFunction(llvm::Function &F) {
  if(F.isIntrinsic())
    return;
  IntrinsicId Id;
  if(getTsarLibFunc(F.getName(), Id)) {
    return;
  }
  //visit all Blocks
  auto& LI = mInstrPass->getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
  mLoopInfo = std::move(LI);
  for(auto &I : F.getBasicBlockList()) {
    visitBasicBlock(I);
  }
  getTypeId(*F.getReturnType());
  //Insert a call of sapforFuncBegin(void*) in the begginning of the function
  auto Fun = getDeclaration(F.getParent(), IntrinsicId::func_begin);
  auto Begin = inst_begin(F);
  std::stringstream Debug;
  Debug << "type=function*file=" << F.getParent()->getSourceFileName() <<
    "*line1=" << F.getSubprogram()->getLine() << "*line2=" <<
    (F.getSubprogram()->getLine() + F.getSubprogram()->getScopeLine()) <<
    "*name1=" << F.getSubprogram()->getName().data() << "*vtype=" <<
    getTypeId(*F.getReturnType()) << "*rank=" <<
    F.getFunctionType()->getNumParams() << "**";
  auto DIFunc = getDbgPoolElem(regDbgStr(Debug.str(), *F.getParent()), *Begin);
  auto Call = CallInst::Create(Fun, {DIFunc}, "");
  Call->insertAfter(DIFunc);
}

void Instrumentation::visitLoadInst(llvm::LoadInst &I) {
  if(!cast<Instruction>(I).getDebugLoc())
    return;
  std::stringstream Debug;
  Debug << "type=file_name*file=" << I.getModule()->getSourceFileName() <<
    "*line1=" << cast<Instruction>(I).getDebugLoc().getLine() << "**";
  auto DILoc = getDbgPoolElem(regDbgStr(Debug.str(), *I.getModule()), I);
  unsigned Idx = mRegistrator.getVarDbgIndex(I.getPointerOperand()
    ->stripPointerCasts()->getName().data(), I.getFunction()->getSubprogram()
    ->getLine(), cast<Instruction>(I).getDebugLoc().getLine());
  auto DIVar = getDbgPoolElem(Idx, *DILoc);
  Function* Fun;
  //FIXME: this doesn't correctly separate arrays from variables
  auto TypeID =  I.getPointerOperand()->getType()->getPointerElementType()
    ->getTypeID();
  //depending on operand type insert a sapforReadVar or sapforReadArr
  //consider all pointers as arrays
  if(TypeID != Type::TypeID::PointerTyID && TypeID != Type::TypeID::ArrayTyID){
    Fun = getDeclaration(I.getModule(), IntrinsicId::read_var);
  } else {
    Fun = getDeclaration(I.getModule(), IntrinsicId::read_arr);
  }
  auto Call = CallInst::Create(Fun, {DILoc, DIVar}, "");
  Call->insertAfter(DILoc);
}

void Instrumentation::visitStoreInst(llvm::StoreInst &I) {
  std::stringstream Debug;
  if(!cast<Instruction>(I).getDebugLoc())
    return;
  Debug << "type=file_name*file=" << I.getModule()->getSourceFileName() <<
    "*line1=" << (!cast<Instruction>(I).getDebugLoc() ? 0 :
    cast<Instruction>(I).getDebugLoc().getLine()) << "**";
  auto DILoc = getDbgPoolElem(regDbgStr(Debug.str(), *I.getModule()), I);
  unsigned Idx = mRegistrator.getVarDbgIndex(I.getPointerOperand()
    ->stripPointerCasts()->getName().data(), I.getFunction()->getSubprogram()
    ->getLine(), cast<Instruction>(I).getDebugLoc().getLine());
  auto DIVar = getDbgPoolElem(Idx, *DILoc);
  Function* Fun;
  //FIXME: this doesn't correctly separate arrays from variables
  auto TypeID =  I.getPointerOperand()->getType()->getPointerElementType()
    ->getTypeID();
  //depending on operand type insert a sapforWriteVarEnd or sapforWriteArrEnd
  //consider all pointers as arrays
  if(TypeID != Type::TypeID::PointerTyID && TypeID != Type::TypeID::ArrayTyID) {
    Fun = getDeclaration(I.getModule(), IntrinsicId::write_var_end);
  } else {
    Fun = getDeclaration(I.getModule(), IntrinsicId::write_arr_end);
  }
  auto Call = CallInst::Create(Fun, {DILoc, DIVar}, "");
  Call->insertAfter(&I);
}

//Registrate given debug information by inserting a call of 
//sapforInitDI(void**, char*). Returns index in pool which is correspond to
//registrated info.
unsigned Instrumentation::regDbgStr(const std::string& S, Module& M) {
  const std::string& ModuleName = M.getModuleIdentifier();
  const std::string& InitFuncName = "sapforInitDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  const std::string& DbgPoolName = "gSapforDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto F = M.getFunction(InitFuncName);
  if(F != nullptr) {
    auto& B = F->getEntryBlock();
    unsigned Val = mRegistrator.regDbgStr();
    auto Fun4Insert = getDeclaration(&M, IntrinsicId::init_di);
    auto Pool = M.getGlobalVariable(DbgPoolName);
    auto Idx = ConstantInt::get(Type::getInt32Ty(M.getContext()), Val);
    auto LoadArr = new LoadInst(Pool, "LoadArr", &B.back());
    auto Elem = GetElementPtrInst::Create(nullptr, LoadArr, {Idx}, "Elem",
      B.getTerminator());
    auto Loc = prepareStrParam(S, B.back());
    CallInst::Create(Fun4Insert, {Elem, Loc}, "", &B.back());
    return Val;
  }
  llvm_unreachable("function was not declared");
}

GetElementPtrInst* Instrumentation::prepareStrParam(const std::string& S,
  Instruction& I) {
  auto Debug = llvm::ConstantDataArray::getString(I.getContext(), S);
  auto Arg = new llvm::GlobalVariable(*I.getModule(), Debug->getType(), true,
    llvm::GlobalValue::InternalLinkage, Debug);
  auto Int0 = llvm::ConstantInt::get(llvm::Type::getInt32Ty(I.getContext()),
    0);
  return llvm::GetElementPtrInst::CreateInBounds(Arg, {Int0,Int0}, "Elem", &I);
}

//Returns element in debug information pool by its index
LoadInst* Instrumentation::getDbgPoolElem(unsigned Val, Instruction& I) {
  const std::string& ModuleName = I.getModule()->getModuleIdentifier();
  const std::string& DbgPoolName = "gSapforDI" + 
    ModuleName.substr(ModuleName.find_last_of("/\\")+1);
  auto Pool = I.getModule()->getGlobalVariable(DbgPoolName);
  auto Idx = ConstantInt::get(Type::getInt32Ty(I.getContext()), Val);
  auto LoadArr = new LoadInst(Pool, "LoadArr", &I);
  auto Elem = GetElementPtrInst::Create(nullptr, LoadArr, {Idx}, "Elem");
  Elem->insertAfter(LoadArr);
  auto DIFunc = new LoadInst(Elem, "DI");
  DIFunc->insertAfter(Elem);
  return DIFunc;
}

void Instrumentation::regGlobals(Module& M) {
  for(auto I = M.global_begin(); I != M.global_end(); I++) {
    unsigned ID = getTypeId(*I->getValueType());
    auto Metadata = getMetadata(&*I);
    if(Metadata == nullptr) {
      return;
    }
    std::stringstream Debug;
    mRegistrator.regVar(Metadata->getName().data(), Metadata->getLine(), true);
    Debug << "type=var_name*file=" << M.getSourceFileName() <<
      "*line1=" << Metadata->getLine() << "*name1=" << 
      Metadata->getName().data() << "*vtype=" << ID << "**";
    regDbgStr(Debug.str(), M);
  }
}

unsigned Instrumentation::getTypeId(const Type& T) {
  switch(T.getTypeID()) {
    case(Type::TypeID::VoidTyID): return BaseTypeID::VoidTy;
    case(Type::TypeID::HalfTyID): return BaseTypeID::HalfTy;
    case(Type::TypeID::FloatTyID): return BaseTypeID::FloatTy;
    case(Type::TypeID::DoubleTyID): return BaseTypeID::DoubleTy;
    case(Type::TypeID::X86_FP80TyID): return BaseTypeID::X86_FP80Ty;
    case(Type::TypeID::FP128TyID): return BaseTypeID::FP128Ty;
    case(Type::TypeID::PPC_FP128TyID): return BaseTypeID::PPC_FP128Ty;
    case(Type::TypeID::LabelTyID): return BaseTypeID::LabelTy;
    case(Type::TypeID::MetadataTyID): return BaseTypeID::MetadataTy;
    case(Type::TypeID::X86_MMXTyID): return BaseTypeID::X86_MMXTy;
    case(Type::TypeID::TokenTyID): return BaseTypeID::TokenTy;
    case(Type::TypeID::FunctionTyID): return BaseTypeID::FunctionTy;
    case(Type::TypeID::PointerTyID): return BaseTypeID::PointerTy;
    case(Type::TypeID::IntegerTyID): 
      if(cast<IntegerType>(T).getBitWidth() < maxIntBitWidth) {
        return (BaseTypeID::IntegerTy + cast<IntegerType>(T).getBitWidth() -1);
      }
    default: return mRegistrator.regType(&T) + BaseTypeID::IntegerTy +
      maxIntBitWidth;
  }
}
