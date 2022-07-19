//===- DIGlobalRetriever.cpp - Global Debug Info Retriever ------*- C++ -*-===//
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
// This file implements a pass which retrieves some debug information for
// global values if it is not presented in LLVM IR.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/KnownFunctionTraits.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Support/Clang/Utils.h"
#include "tsar/Transform/Mixed/Passes.h"
#include <bcl/utility.h>
#include <clang/Basic/LangOptions.h>
#include <clang/AST/Decl.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DIBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/FileSystem.h>

using namespace clang;
using namespace llvm;
using namespace tsar;

namespace {
/// This retrieves some debug information for global values if it is not
/// presented in LLVM IR ('sapfor.dbg' metadata will be attached to globals).
class DINodeRetrieverPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;

  DINodeRetrieverPass() : ModulePass(ID) {
    initializeDINodeRetrieverPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(llvm::Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.getPreservesAll();
  }

private:
  /// Insert artificial metadata for allocas.
  void insertDeclareIfNotExist(Function &F, DIFile *FileCU, DIBuilder &DIB) {
    for (auto &I : instructions(F))
      if (auto *AI = dyn_cast<AllocaInst>(&I)) {
        auto &Ctx = AI->getContext();
        auto AddrUses = FindDbgAddrUses(AI);
        if (!AddrUses.empty()) {
          for (auto *DbgInst : AddrUses) {
            // Artificial variables without name may produce the same
            // metdata-level locations for distinct lower level locations.
            // This leads to incorrect metadata-level alias tree.
            // So, replace this variable with the distinct one.
            if (auto DIVar = DbgInst->getVariable())
              if (DIVar->getFlags() & DINode::FlagArtificial &&
                  DIVar->getName().empty()) {
                auto DistinctDIVar = DILocalVariable::getDistinct(
                    Ctx, DIVar->getScope(), "sapfor.var", DIVar->getFile(),
                    DIVar->getLine(), DIVar->getType(), DIVar->getArg(),
                    DIVar->getFlags(), DIVar->getAlignInBits(), nullptr);
                DbgInst->setVariable(DistinctDIVar);
            }
          }
          continue;
        }
        auto *DITy =
          createStubType(*F.getParent(), AI->getType()->getAddressSpace(), DIB);
        auto *DISub = F.getSubprogram();
        auto *DIVar = DILocalVariable::getDistinct(
            Ctx, DISub, "sapfor.var", FileCU, 0, DITy, 0,
            DINode::FlagArtificial, AI->getAlign().value(), nullptr);
        DIB.insertDeclare(AI, DIVar, DIExpression::get(Ctx, {}),
            DILocation::get(AI->getContext(), 0, 0, DISub), AI->getParent());
      }
  }
};
} // namespace

char DINodeRetrieverPass::ID = 0;
INITIALIZE_PASS(DINodeRetrieverPass, "di-node-retriever",
                "Debug Info Retriever", true, false)

bool DINodeRetrieverPass::runOnModule(llvm::Module &M) {
  auto *TEP{getAnalysisIfAvailable<TransformationEnginePass>()};
  auto &Ctx = M.getContext();
  auto CUItr = M.debug_compile_units_begin();
  assert(CUItr != M.debug_compile_units_end() &&
         "At least one compile unit must be available!");
  auto CU =
    std::distance(CUItr, M.debug_compile_units_end()) == 1 ? *CUItr : nullptr;
  SmallString<256> CWD;
  auto DirName = CU ? CUItr->getDirectory()
                    : (llvm::sys::fs::current_path(CWD), StringRef(CWD));
  auto *FileCU = CU ? CU->getFile() : nullptr;
  DIBuilder DIB(M);
  for (auto &GlobalVar : M.globals()) {
    SmallVector<DIMemoryLocation, 1> DILocs;
    if (findGlobalMetadata(&GlobalVar, DILocs))
      continue;
    DIFile *File = FileCU;
    unsigned Line = 0;
    // A name should be specified for global variables, otherwise LLVM IR is
    // considered corrupted.
    StringRef Name = "sapfor.var";
    if (TEP && *TEP)
      for (auto [CU, TfmCtxBase] : (*TEP)->contexts())
        if (auto *TfmCtx{dyn_cast<ClangTransformationContext>(TfmCtxBase)})
          if (auto D = TfmCtx->getDeclForMangledName(GlobalVar.getName())) {
            auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
            auto FName =
                SrcMgr.getFilename(SrcMgr.getExpansionLoc(D->getBeginLoc()));
            File = DIB.createFile(FName, CU->getDirectory());
            Line = SrcMgr.getPresumedLineNumber(
                SrcMgr.getExpansionLoc(D->getBeginLoc()));
            if (auto ND = dyn_cast<NamedDecl>(D))
              Name = ND->getName();
            break;
          }
    auto *DITy = createStubType(M, GlobalVar.getType()->getAddressSpace(), DIB);
    auto *GV = DIGlobalVariable::getDistinct(
      Ctx, File, Name, GlobalVar.getName(), File, Line, DITy,
      GlobalVar.hasLocalLinkage(), !GlobalVar.isDeclaration(),
      nullptr, nullptr, 0, nullptr);
    auto *GVE =
      DIGlobalVariableExpression::get(Ctx, GV, DIExpression::get(Ctx, {}));
    GlobalVar.setMetadata("sapfor.dbg", GVE);
  }
  for (auto &F : M.functions()) {
    if (F.getSubprogram()) {
      insertDeclareIfNotExist(F, FileCU, DIB);
      continue;
    }
    if (F.isIntrinsic() && (isDbgInfoIntrinsic(F.getIntrinsicID()) ||
        isMemoryMarkerIntrinsic(F.getIntrinsicID())))
      continue;
    DIFile *File = FileCU;
    unsigned Line = 0;
    auto Flags = DINode::FlagZero;
    MDString *Name = nullptr;
    if (TEP && *TEP)
      for (auto [CU, TfmCtxBase] : (*TEP)->contexts())
        if (auto *TfmCtx{dyn_cast<ClangTransformationContext>(TfmCtxBase)})
          if (auto *D = TfmCtx->getDeclForMangledName(F.getName())) {
            auto &SrcMgr = TfmCtx->getRewriter().getSourceMgr();
            auto FName =
                SrcMgr.getFilename(SrcMgr.getExpansionLoc(D->getBeginLoc()));
            File = DIB.createFile(FName, CU->getDirectory());
            Line = SrcMgr.getPresumedLineNumber(
                SrcMgr.getExpansionLoc(D->getBeginLoc()));
            if (auto *FD = dyn_cast<FunctionDecl>(D)) {
              SmallString<64> ExtraName;
              Name = MDString::get(Ctx, getFunctionName(*FD, ExtraName));
              if (FD->hasPrototype())
                Flags |= DINode::FlagPrototyped;
              if (FD->isImplicit())
                Flags |= DINode::FlagArtificial;
            }
          }
    auto SPFlags =
        DISubprogram::toSPFlags(F.hasLocalLinkage(), !F.isDeclaration(), false);
    auto *SP = DISubprogram::getDistinct(Ctx, File, Name,
      MDString::get(Ctx, F.getName()), File, Line, nullptr, Line, nullptr,
      0, 0, Flags, SPFlags, !F.isDeclaration() ? CU : nullptr);
    F.setMetadata("sapfor.dbg", SP);
    insertDeclareIfNotExist(F, FileCU, DIB);
  }
  return true;
}

ModulePass *llvm::createDINodeRetrieverPass() {
  return new DINodeRetrieverPass();
}
