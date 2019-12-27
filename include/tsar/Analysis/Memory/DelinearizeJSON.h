//===-- DelinearizeJSON.h -------- TSAR Attributes --------------*- C++ -*-===//
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
// This file describes delinearized array accesses in JSON format.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_DELINEARIZE_JSON_H
#define TSAR_DELINEARIZE_JSON_H

#include <bcl/Json.h>

namespace llvm {
class ScalarEvolution;
}

namespace tsar {
class DelinearizeInfo;

/// Description of delinearized array accesses.
JSON_OBJECT_BEGIN(RawDelinearizeInfo)
JSON_OBJECT_ROOT_PAIR_3(RawDelinearizeInfo,
  Accesses, std::map<BCL_JOIN(std::string,
    std::vector<std::vector<std::vector<std::string>>>)>,
  Sizes, std::map<BCL_JOIN(std::string, std::vector<std::string>)>,
  IsDelinearized, int)

  RawDelinearizeInfo() : JSON_INIT_ROOT{}

  RawDelinearizeInfo(const RawDelinearizeInfo &) = default;
  RawDelinearizeInfo & operator=(const RawDelinearizeInfo &) = default;
  RawDelinearizeInfo(RawDelinearizeInfo &&) = default;
  RawDelinearizeInfo & operator=(RawDelinearizeInfo &&) = default;
JSON_OBJECT_END(RawDelinearizeInfo)

/// Builds JSON representation for delinearized array accesses.
RawDelinearizeInfo toJSON(const DelinearizeInfo &Info,
  llvm::ScalarEvolution &SE, bool IsSafeTypeCast = true);
}
JSON_DEFAULT_TRAITS(tsar::, RawDelinearizeInfo)
#endif//TSAR_DELINEARIZE_JSON_H
