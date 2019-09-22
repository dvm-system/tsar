//===----- MetadataUtils.cpp - Utils for exploring metadata -----*- C++ -*-===//
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
// This file implements helpful functions to create and access metadata.
//
//===----------------------------------------------------------------------===//

#include "tsar/Transform/IR/MetadataUtils.h"
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
