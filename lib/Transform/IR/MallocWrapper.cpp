#include "tsar/Transform/IR/Passes.h"
#include "tsar/Analysis/KnownFunctionTraits.h"
#include <bcl/utility.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/Pass.h>
#include <llvm/Support/Debug.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <string>
#include <map>
#include <sstream>

using namespace llvm;
using namespace tsar;

namespace
{
class MallocWrapperPass: public ModulePass, private bcl::Uncopyable {
  std::map<std::string, FunctionCallee> wrappers_map;

  FunctionCallee createMallocWrapper(Module &M, Type *type);
  std::string getWrapperName(Type *type);
public:
  static char ID;
  MallocWrapperPass() : ModulePass(ID) {
    initializeMallocWrapperPassPass(*PassRegistry::getPassRegistry());
  }
  bool runOnModule(Module &M) override;
};
}

#undef DEBUG_TYPE
#define DEBUG_TYPE "wrap-malloc"

char MallocWrapperPass::ID = 0;
INITIALIZE_PASS(MallocWrapperPass, "wrap-malloc",
  "Wraps malloc", false, false)

ModulePass * llvm::createMallocWrapperPass() {
  return new MallocWrapperPass(); 
}

std::string MallocWrapperPass::getWrapperName(Type *type) {
  std::string s;
  raw_string_ostream ostream(s);
  type->print(ostream);

  return std::string("malloc_wrapper_") + ostream.str();
}

FunctionCallee MallocWrapperPass::createMallocWrapper(Module &M, Type *type) { 
  std::string name = getWrapperName(type);
  if (wrappers_map.count(name) != 0) {
    return wrappers_map[name];
  }

  auto& context = M.getContext();

  wrappers_map[name] = M.getOrInsertFunction(name, type, Type::getInt64Ty(context));
  Function *f = cast<Function> (wrappers_map[name].getCallee());
  f->setCallingConv(CallingConv::C);
  f->addAttribute(AttributeList::ReturnIndex, Attribute::NoAlias);
  Function::arg_iterator args = f->arg_begin();

  Value *size = args++;
  size->setName("size");

  BasicBlock *block = BasicBlock::Create(context, "entry", f);
  IRBuilder<> builder(block);

  auto malloc_func = M.getOrInsertFunction("malloc", Type::getInt8PtrTy(context), Type::getInt64Ty(context));

  CallInst *mem = builder.CreateCall(malloc_func, size);
  mem->addAttribute(AttributeList::ReturnIndex, Attribute::NoAlias);
  Value *result = builder.CreatePointerCast(mem, type, "result");
  builder.CreateRet(result);

  return wrappers_map[name];
}

bool MallocWrapperPass::runOnModule(Module &M) {
  std::vector<std::pair<CallInst*, Type*>> malloc_call_list;

  for (auto &F: M) {
    for (auto &BB: F) {
      for (auto &I: BB) {
        if (I.getOpcode() == Instruction::Call) {
          CallInst *callI = cast<CallInst>(&I);
          auto func_name = callI->getCalledFunction()->getName();
          if (func_name != "malloc") {
            break;
          }


          Type *t = nullptr;
          for (auto &use: callI->uses()) {
            Instruction *inst = cast<Instruction>(use.getUser());
            if (!inst || inst->getOpcode() != Instruction::BitCast) { break; }
            if (!t) {
              t = inst->getType();
            } else if (t != use->getType()) {
              t = nullptr;
              break;
            }
          }

          if (t) {
            malloc_call_list.push_back({callI, t});
          }
        }
      }
    }
  }

  for (auto &el: malloc_call_list) {
    auto callI = el.first;
    auto t = el.second;

    auto fc = createMallocWrapper(M, t);
    CallInst* inst_call_new = CallInst::Create(fc, callI->getOperand(0));
    inst_call_new->addAttribute(AttributeList::ReturnIndex, Attribute::NoAlias);
    ReplaceInstWithInst(callI, inst_call_new);

    for (auto &use: callI->uses()) {
      Value * res_new = cast<Value>(inst_call_new);
      ReplaceInstWithInst(cast<Instruction>(use.getUser()), BitCastInst::CreatePointerCast(res_new, res_new->getType()));
    }
  }

  for (auto &F : M) {
    if (!F.empty()) {
      errs() << F << "\n";
    }
  }

  return true;
}