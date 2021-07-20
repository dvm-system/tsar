//===--- ServerUtils.cpp ---- Analysis Server Utils -------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2019 DVM System Group
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
//===----------------------------------------------------------------------===//
//
// This file implements functions to simplify client to server data mapping.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/ServerUtils.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryEnvironment.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Support/MetadataUtils.h"
#include <llvm/ADT/DenseMap.h>
#include <llvm/IR/InstIterator.h>
#include "llvm/IR/InstVisitor.h"
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>

#define DEBUG_TYPE "di-memory-handle"

using namespace llvm;
using namespace tsar;

void ClientToServerMemory::prepareToClone(
    llvm::Module &ClientM, llvm::ValueToValueMapTy &ClientToServer) {
  // By default global metadata variables and some of local variables are not
  // cloned. This leads to implicit references to the original module.
  // For example, traverse of MetadataAsValue for the mentioned variables
  // visits DbgInfo intrinsics in both modules (clone and origin).
  // So, we perform preliminary manual cloning of local variables.
  for (auto &F : ClientM) {
    for (auto &I : instructions(F))
      if (auto DDI = dyn_cast<DbgVariableIntrinsic>(&I)) {
        MapMetadata(cast<MDNode>(DDI->getVariable()), ClientToServer);
        SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
        DDI->getAllMetadata(MDs);
        for (auto MD : MDs)
          MapMetadata(MD.second, ClientToServer);
      }
    // Cloning of a function do not duplicate DISubprogram metadata, however
    // cloning of variables implemented above already duplicate this metadata.
    // So, we revert this cloning to prevent changes of behavior of other
    // cloning methods. For example, cloned metadata-level memory locations
    // must point to the original function because after rebuilding alias tree
    // metadata attached to a function will be used and this metadata is always
    // points to the original DISubprogram.
    if (auto *MD = findMetadata(&F))
      if (ClientToServer.getMDMap())
        (*ClientToServer.getMDMap())[MD].reset(MD);
  }
}

namespace {
struct DICompileUnitSearch {
  void visitMDNode(MDNode &MD) {
    if (!MDNodes.insert(&MD).second)
      return;
    for (Metadata *Op : MD.operands()) {
      if (!Op)
        continue;
      if (auto *CU = dyn_cast<DICompileUnit>(Op))
        CUs.push_back(CU);
      if (auto *N = dyn_cast<MDNode>(Op)) {
        visitMDNode(*N);
        continue;
      }
    }
  }

  SmallPtrSet<const Metadata *, 32> MDNodes;
  SmallVector<DICompileUnit *, 2> CUs;
};

DILocalScope *cloneWithNewSubprogram(DILocalScope *Scope, DISubprogram *Sub) {
  if (!Scope)
    return nullptr;
  if (isa<DISubprogram>(Scope))
    return Sub;
  auto Clone{Scope->clone()};
  for (unsigned I = 0, EI = Clone->getNumOperands(); I < EI; ++I)
    if (auto Parent = dyn_cast_or_null<DILocalScope>(Clone->getOperand(I))) {
      assert(cast<DILocalScope>(*Clone).getScope() == Parent &&
             "Unknown reference to local scope!");
      Clone->replaceOperandWith(I, cloneWithNewSubprogram(Parent, Sub));
    }
  // Replace temporary node with a permanent one after all operands are
  // cloned. Otherwise, metadata in the original module may be corrupted.
  return cast<DILocalScope>(
      MDNode::replaceWithPermanent<MDNode>(std::move(Clone)));
}

DILocation *cloneWithNewSubprogram(
    DILocation *Loc,
    const DenseMap<DISubprogram *, DISubprogram *> &SubprogramMap) {
  if (!Loc)
    return Loc;
  bool NeedToClone = false;
  auto *Scope = Loc->getScope();
  auto *DISub = getSubprogram(Scope);
  auto MapItr = SubprogramMap.find(DISub);
  if (MapItr != SubprogramMap.end()) {
    NeedToClone = true;
    Scope = cloneWithNewSubprogram(Scope, MapItr->second);
  }
  auto *InlineAt = Loc->getInlinedAt();
  if (InlineAt) {
    auto CloneInlineAt = cloneWithNewSubprogram(InlineAt, SubprogramMap);
    if (CloneInlineAt != InlineAt) {
      NeedToClone = true;
      InlineAt = CloneInlineAt;
    }
  }
  if (!NeedToClone)
    return Loc;
  return DILocation::get(Loc->getContext(), Loc->getLine(), Loc->getColumn(),
                         Scope, InlineAt);
}


MDNode *cloneWithNewSubprogram(
    MDNode *MD, const DenseMap<DISubprogram *, DISubprogram *> &SubprogramMap,
    llvm::ValueToValueMapTy &ClientToServer,
    SmallPtrSetImpl<MDNode *> &Visited) {
  if (!Visited.insert(MD).second)
    return MD;
  bool NeedToClone = false;
  for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I) {
    auto &Op = MD->getOperand(I);
    if (!Op)
      continue;
    if (auto *DISub = dyn_cast<DISubprogram>(Op)) {
      auto MapItr = SubprogramMap.find(DISub);
      if (MapItr != SubprogramMap.end())
        if (!ClientToServer.getMappedMD(MD))
          MD->replaceOperandWith(I, MapItr->second);
        else
          NeedToClone = true;
    } else if (auto MDOp = dyn_cast<MDNode>(Op)) {
      auto NewMD = cloneWithNewSubprogram(MDOp, SubprogramMap, ClientToServer,
                                            Visited);
      if (MDOp != NewMD)
        if (!ClientToServer.getMappedMD(MD))
          MD->replaceOperandWith(I, NewMD);
        else
          NeedToClone = true;
    }
  }
  if (NeedToClone) {
    ClientToServer.MD().erase(MD);
    return MapMetadata(MD, ClientToServer);
  }
  return MD;
}

MDNode * cloneWithNewSubprogram(
    MDNode *MD, const DenseMap<DISubprogram *, DISubprogram *> &SubprogramMap,
    llvm::ValueToValueMapTy &ClientToServer) {
  SmallPtrSet<MDNode *, 8> Visited;
  return cloneWithNewSubprogram(MD, SubprogramMap, ClientToServer, Visited);
}

#ifdef LLVM_DEBUG
void checkFuncDescription(llvm::Module &M, const Twine &Prefix) {
  for (auto &F : M)
    for (auto &I : instructions(&F))
      if (auto &Loc{I.getDebugLoc()})
        if (auto *Scope{Loc->getScope()})
          if (auto *DISub{Scope->getSubprogram()})
        if (!DISub->describes(&F)) {
          dbgs() << Prefix;
          dbgs()
              << "function-to-subprogram mistmach at location attached to ";
          I.print(dbgs());
          Loc.print(dbgs());
          dbgs() << ": function " << F.getName() << " subprogram: ";
          DISub->print(dbgs());
          dbgs() << "\n";
        }
}
#endif
}

void ClientToServerMemory::initializeServer(
    llvm::Pass &P, llvm::Module &ClientM, llvm::Module &ServerM,
    llvm::ValueToValueMapTy &ClientToServer, llvm::legacy::PassManager &PM) {
  /// Manually attach sapfor.dbg metadata to cloned objects.
  for (auto &ClientF : ClientM) {
    auto F = ClientToServer[&ClientF];
    assert(F && "Mapped function for a specified one must exist!");
    if (!findMetadata(cast<Function>(F)))
      if (auto *ClientMD = findMetadata(&ClientF)) {
        auto MD = ClientToServer.getMappedMD(ClientMD);
        assert(MD && "Mapped metadata for a subprogram must exist!");
        cast<Function>(F)->setMetadata("sapfor.dbg", cast<MDNode>(*MD));
      }
  }
  // Collect copies of DISubprogram metadata. This copies were implicitly
  // created while local variables were copying.
  DenseMap<DISubprogram *, DISubprogram *> SubprogramMap;
  for (auto &Pair : *ClientToServer.getMDMap())
    if (Pair.second && Pair.first != Pair.second)
      if (auto *FromDIV = dyn_cast<DILocalVariable>(Pair.first)) {
        auto *FromScope = getSubprogram(FromDIV->getScope());
        assert(FromScope && "Local variable is not attached to a subprogram!");
        auto *ToScope =
            getSubprogram(cast<DILocalVariable>(Pair.second)->getScope());
        assert(ToScope && "Local variable is not attached to a subprogram!");
        SubprogramMap.try_emplace(cast<DISubprogram>(FromScope),
                                  cast<DISubprogram>(ToScope));
      }
  // Replace DISubprograms with their copies in a new module.
  for (auto &Pair : *ClientToServer.getMDMap())
    if (Pair.second && Pair.first != Pair.second)
      if (auto *MD = dyn_cast<MDNode>(Pair.second))
        for (unsigned I = 0, EI = MD->getNumOperands(); I < EI; ++I)
          if (auto Op = dyn_cast_or_null<DISubprogram>(MD->getOperand(I))) {
            auto MapItr = SubprogramMap.find(Op);
            if (MapItr != SubprogramMap.end())
              MD->replaceOperandWith(I, MapItr->second);
          }
  for (auto &Pair : SubprogramMap)
    ClientToServer.MD()[Pair.first].reset(Pair.second);
  for (auto &F : ServerM) {
    if (auto *DISub = F.getSubprogram()) {
      auto MapItr = SubprogramMap.find(DISub);
      if (MapItr != SubprogramMap.end())
        F.setSubprogram(MapItr->second);
    }
    for (auto &I : instructions(&F)) {
      if (auto &Loc = I.getDebugLoc()) {
        auto *NewLoc = cloneWithNewSubprogram(Loc.get(), SubprogramMap);
        if (NewLoc != Loc)
          I.setDebugLoc(NewLoc);
      }
      if (auto *MD = I.getMetadata(LLVMContext::MD_loop))
        for (unsigned I = 1; I < MD->getNumOperands(); ++I)
          if (auto *Loc = dyn_cast_or_null<DILocation>(MD->getOperand(I))) {
            auto *NewLoc = cloneWithNewSubprogram(Loc, SubprogramMap);
            if (NewLoc != Loc)
              MD->replaceOperandWith(I, NewLoc);
          }
    }
  }
  // Add list DICompileUnits (this may occur due to manual mapping performed
  // in prepareToClone().
  // TODO (kaniandr@gmail.com): it seems that the appropriate check will be
  // added in the next LLVM release (in 10 version). So, remove this check.
  auto *ServerCUs = ServerM.getOrInsertNamedMetadata("llvm.dbg.cu");
  SmallPtrSet<const void *, 8> Visited;
  for (const auto *CU : ServerCUs->operands())
    Visited.insert(CU);
  DICompileUnitSearch Search;
  for (auto &F : ServerM) {
    SmallVector<std::pair<unsigned, MDNode *>, 1> MDs;
    F.getAllMetadata(MDs);
    for (auto &MD : MDs)
      Search.visitMDNode(*MD.second);
  }
  for (auto *CU : Search.CUs)
    if (Visited.insert(CU).second)
      ServerCUs->addOperand(CU);
  LLVM_DEBUG(checkFuncDescription(ClientM, "Error (on client): "));
  LLVM_DEBUG(checkFuncDescription(ServerM, "Error (on server): "));
  // Prepare to mapping from origin to cloned DIMemory.
  auto &Env = P.getAnalysis<DIMemoryEnvironmentWrapper>();
  MDToDIMemoryMap CloneToOrigin;
  for (auto &ClientF: ClientM) {
    auto DIAT = Env->get(ClientF);
    if (!DIAT)
      continue;
    auto F = ClientToServer[&ClientF];
    SmallDenseMap<MDNode *, MDNode *, 8> DIMReplacement;
    assert(F && "Mapped function for a specified one must exist!");
    LLVM_DEBUG(dbgs() << "[CLONED DI MEMORY]: create mapping for function '"
                      << F->getName() << "'\n");
    for (auto &DIM : make_range(DIAT->memory_begin(), DIAT->memory_end())) {
      LLVM_DEBUG(dbgs() << "[CLONED DI MEMORY]: add to map: ";
                 if (auto DWLang = getLanguage(ClientF))
                     printDILocationSource(*DWLang, DIM, dbgs());
                 dbgs() << "\n");
      auto MD = ClientToServer.getMappedMD(DIM.getAsMDNode());
      assert(MD && "Mapped metadata for a specified memory must exist!");
      auto ServerMD = cloneWithNewSubprogram(cast<MDNode>(*MD), SubprogramMap,
                                             ClientToServer);
      if (*MD != ServerMD)
        DIMReplacement.try_emplace(cast<MDNode>(*MD), ServerMD);
      CloneToOrigin.try_emplace(std::make_pair(cast<Function>(F), ServerMD),
                                &DIM);
    }
    if (!DIMReplacement.empty()) {
      auto AliasTreeMD = cast<Function>(F)->getMetadata("alias.tree");
      assert(AliasTreeMD && "List of alias tree nodes must not be null!");
      if (ClientToServer.getMappedMD(AliasTreeMD)) {
        ClientToServer.MD().erase(AliasTreeMD);
        MapMetadata(AliasTreeMD, ClientToServer);
      } else {
        for (unsigned I = 0, EI = AliasTreeMD->getNumOperands(); I < EI; ++I) {
          auto ReplacementItr =
              DIMReplacement.find(cast<MDNode>(AliasTreeMD->getOperand(I)));
          if (ReplacementItr != DIMReplacement.end())
            AliasTreeMD->replaceOperandWith(I, ReplacementItr->second);
        }
      }
    }
  }
  // Passes are removed in backward direction and handlers should be removed
  // before memory environment.
  PM.add(createGlobalsAccessStorage());
  PM.add(createClonedDIMemoryMatcherStorage());
  PM.add(createDIMemoryEnvironmentStorage());
  PM.add(createGlobalsAccessCollector());
  PM.add(createClonedDIMemoryMatcher(std::move(CloneToOrigin)));
}

namespace llvm {
static void initializeDestroyCSMemoryHandlesPassPass(PassRegistry &Registry);
}

namespace {
/// Allow server to force destruction of memory handles which is used
/// to track metadata-level memory locations on client.
///
/// Handles should be destroyed before metadata-level memory environment is
/// destroyed. However, if connection is closed before a such destruction the
/// order of further destructions is not specified. This may lead to undefined
/// behavior including analyzer aborting.
class DestroyCSMemoryHandlesPass : public ModulePass, private bcl::Uncopyable {
public:
  static char ID;
  DestroyCSMemoryHandlesPass() : ModulePass(ID) {
    initializeDestroyCSMemoryHandlesPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &) override {
    auto & Matcher = getAnalysis<ClonedDIMemoryMatcherWrapper>();
    Matcher->clear();
    return false;
  }

  void getAnalysisUsage(AnalysisUsage & AU) const override {
    AU.addRequired<ClonedDIMemoryMatcherWrapper>();
    AU.setPreservesAll();
  }
};
}

char DestroyCSMemoryHandlesPass::ID = 0;
INITIALIZE_PASS_BEGIN(DestroyCSMemoryHandlesPass, "cs-close-di-memory-matcher",
                      "Destroy Client To Server Memory Handles", true, true)
INITIALIZE_PASS_DEPENDENCY(ClonedDIMemoryMatcherWrapper)
INITIALIZE_PASS_END(DestroyCSMemoryHandlesPass, "cs-close-di-memory-matcher",
                    "Destroy Client To Server Memory Handles", true, true)

void ClientToServerMemory::prepareToClose(legacy::PassManager &PM) {
  PM.add(new DestroyCSMemoryHandlesPass());
}

void ClientToServerMemory::getAnalysisUsage(AnalysisUsage& AU) {
  AU.addRequired<DIMemoryEnvironmentWrapper>();
}
