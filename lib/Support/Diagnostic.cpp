//===---- Diagnostic.h -- Low-level Diagnostic Handling ---------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file implements functions to emit different TSAR-embeded diagnostics.
//
//===----------------------------------------------------------------------===//


#include "tsar/Support/Diagnostic.h"
#include <string>

static const tsar::DiagnosticInfo Infos[] = {
#define DIAG(ENUM,LEVEL,DESC) \
  tsar::DiagnosticInfo{ \
    tsar::DiagnosticInfo::LEVEL, \
    DESC, \
    std::char_traits<char>::length(DESC) \
  },
#include "tsar/Support/DiagnosticKinds.inc"
#undef DIAG
};

const tsar::DiagnosticInfo *
tsar::getDiagnosticInfo(unsigned DiagId) {
  if (DiagId <= tsar::diag::PADDING_BUILTIN_TSAR_DIAGNOSTIC ||
      DiagId >= tsar::diag::INVALID_BUILTIN_TSAR_DIAGNOSTIC)
    return nullptr;
  return &Infos[DiagId - tsar::diag::PADDING_BUILTIN_TSAR_DIAGNOSTIC - 1];
}