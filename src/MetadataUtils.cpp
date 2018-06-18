//===----- MetadataUtils.cpp - Utils for exploring metadata -----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file implements helpful functions to create and access metadata.
//
//===----------------------------------------------------------------------===//

#include "MetadataUtils.h"
#include <llvm/IR/Module.h>
#include <llvm/IR/GlobalObject.h>

using namespace llvm;

namespace tsar {
void addNameDAMetadata(GlobalObject &GO, StringRef NamedMD,
    StringRef ValueKind, ArrayRef<Metadata *> MDs) {
  assert(GO.getParent() && "Function must be inserted into a module!");
  auto GOKindMD = MDString::get(GO.getContext(),  ValueKind);
  auto *GORefMD = ValueAsMetadata::get(&GO);
  SmallVector<Metadata *, 2> Ops({ GOKindMD, GORefMD });
  Ops.append(MDs.begin(), MDs.end());
  auto GOMD = MDNode::get(GO.getContext(), Ops);
  GO.setMetadata(NamedMD, GOMD);
  auto NamedMDPtr = GO.getParent()->getOrInsertNamedMetadata(NamedMD);
  NamedMDPtr->addOperand(GOMD);
}

Optional<unsigned> eraseFromParent(Module &M, StringRef NamedMD,
    StringRef MD) {
  if (auto NamedMDPtr = M.getNamedMetadata(NamedMD))
    if (auto MDPtr = getMDOfKind(*NamedMDPtr, MD)) {
      auto Item = extractMD<GlobalObject>(*MDPtr);
      if (Item.first) {
        Item.first->eraseFromParent();
        return Item.second;
      }
    }
  return None;
}

static MDString * getMDKindString(MDNode &MD) {
  for (auto &Op : MD.operands()) {
    if (auto *Str = dyn_cast<MDString>(Op))
      return Str;
  }
  return nullptr;
}

MDNode * getMDOfKind(NamedMDNode &NamedMD, StringRef Kind) {
  for (auto *MD : NamedMD.operands())
    if (getMDKindString(*MD)->getString() == Kind)
      return MD;
  return nullptr;
}
}