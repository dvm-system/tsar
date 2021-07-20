//===-- Attributes.h -------- TSAR Attributes -------------------*- C++ -*-===//
//
//                     Traits Static Analyzer (SAPFOR)
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
// This file defines a enum of attributes supported by TSAR. Some helpful
// methods to process these attributes are also defined.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_ATTRIBUTES_H
#define TSAR_ATTRIBUTES_H

#include <llvm/ADT/StringRef.h>

namespace llvm {
class Function;
}

namespace tsar {
/// This enum contains a value for every attribute known by TSAR.
enum class AttrKind : unsigned {
  not_attribute = 0,
#define GET_ATTRIBUTE_ENUM_VALUES
#include "tsar/Analysis/Attributes.gen"
#undef GET_ATTRIBUTE_ENUM_VALUES
  num_attributes
};

/// Returns string representation of the attribute.
llvm::StringRef getAsString(AttrKind Kind);

/// Adds function attributes to a specified function.
void addFnAttr(llvm::Function &F, AttrKind Kind,
  llvm::StringRef Val = llvm::StringRef());

/// Removes function attributes from a specified function.
void removeFnAttr(llvm::Function &F, AttrKind Kind);

/// Returns true if the function has a specified attribute.
bool hasFnAttr(const llvm::Function &F, AttrKind Kind);

/// Applies the given function object to each attribute.
template<class Function> void for_each_attr(Function &&F) {
  auto KindI = static_cast<unsigned>(AttrKind::not_attribute);
  auto KindE = static_cast<unsigned>(AttrKind::num_attributes);
  for (KindI++; KindI < KindE; ++KindI)
    F(static_cast<AttrKind>(KindI));
}

/// Returns true if a specified name is a name of input/output library function.
bool isIOLibFuncName(llvm::StringRef FuncName);
}
#endif//TSAR_ATTRIBUTES_H
