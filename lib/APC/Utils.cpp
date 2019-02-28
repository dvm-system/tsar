//===--- Utils.cpp ------ Utility Methods and Classes  ----------*- C++ -*-===//
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
// This file implements abstractions which simplify usage of other abstractions.
//
//===----------------------------------------------------------------------===//

#include "tsar/APC/Utils.h"
#include <apc/apc-config.h>
#include <llvm/Support/raw_os_ostream.h>

namespace tsar {
void printAPCVersion(llvm::raw_ostream &OS) {
  OS << "  APC (" << APC_HOMEPAGE_URL << "):\n";
  OS << "    version " << APC_VERSION_STRING << "\n";
  OS << "    language ";
#ifdef APC_C
  OS << "C";
#endif
  OS << "\n";
}
}
