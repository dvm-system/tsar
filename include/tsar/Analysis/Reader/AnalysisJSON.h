//===---- AnalysisJSON.h --- Analysis Results In JSON  -----------*- C++ -*===//
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
// This file defines representation of analysis results in JSON format.
//
//===----------------------------------------------------------------------===//

#ifndef ANALYSIS_JSON_H
#define ANALYSIS_JSON_H

#include "tsar/Analysis/Memory/MemoryTrait.h"
#include <bcl/Json.h>

namespace tsar{
namespace trait {
using LineTy = unsigned;
using ColumnTy = unsigned;
using DistanceTy = std::optional<int>;
using IdTy = std::size_t;

/// Definition of a JSON-object which represents a function.
JSON_OBJECT_BEGIN(Function)
JSON_OBJECT_PAIR_5(Function
  , File, std::string
  , Line, LineTy
  , Column, ColumnTy
  , Name, std::string
  , Pure, bool)
JSON_OBJECT_END(Function)


/// Definition of a JSON-object which represents a variable.
JSON_OBJECT_BEGIN(Var)
JSON_OBJECT_PAIR_4(Var,
  File, std::string,
  Line, LineTy,
  Column, ColumnTy,
  Name, std::string)
JSON_OBJECT_END(Var)

/// Definition of a JSON-object which represents a distance.
JSON_OBJECT_BEGIN(Distance)
  JSON_OBJECT_PAIR_2(Distance, Min, DistanceTy, Max, DistanceTy)
  Distance() : JSON_INIT(Distance, std::nullopt, std::nullopt) {}
  Distance(DistanceTy Min, DistanceTy Max) : JSON_INIT(Distance, Min, Max) {}
JSON_OBJECT_END(Distance)

/// Definition of a JSON-object which represents a range.
JSON_OBJECT_BEGIN(Range)
JSON_OBJECT_PAIR_3(Range
  , Start, DistanceTy
  , End, DistanceTy
  , Step, DistanceTy)
Range() : JSON_INIT(Range, std::nullopt, std::nullopt, std::nullopt) {}
Range(DistanceTy Start, DistanceTy End, DistanceTy Step)
    : JSON_INIT(Range, Start, End, Step) {}
JSON_OBJECT_END(Range)

/// Definition of a JSON-object which represents a loop and its properties.
JSON_OBJECT_BEGIN(Loop)
JSON_OBJECT_PAIR_13(Loop,
  File, std::string,
  Line, LineTy,
  Column, ColumnTy,
  Private, std::set<IdTy>,
  Reduction, std::map<BCL_JOIN(IdTy, trait::Reduction::Kind)>,
  Induction, std::map<BCL_JOIN(IdTy, Range)>,
  Flow, std::map<BCL_JOIN(IdTy, Distance)>,
  Anti, std::map<BCL_JOIN(IdTy, Distance)>,
  Output, std::set<IdTy>,
  WriteOccurred, std::set<IdTy>,
  ReadOccurred, std::set<IdTy>,
  UseAfterLoop, std::set<IdTy>,
  DefBeforeLoop, std::set<IdTy>)
JSON_OBJECT_END(Loop)

/// Definition of a top-level JSON-object with name 'Info', which contains
/// list of variables and loops.
JSON_OBJECT_BEGIN(Info)
  JSON_OBJECT_ROOT_PAIR_3(Info
   , Functions, std::vector<trait::Function>
   , Vars, std::vector<trait::Var>
   , Loops, std::vector<trait::Loop>
  )
  Info() : JSON_INIT_ROOT{}
JSON_OBJECT_END(Info)
}
}

JSON_DEFAULT_TRAITS(tsar::trait::, Function)
JSON_DEFAULT_TRAITS(tsar::trait::, Var)
JSON_DEFAULT_TRAITS(tsar::trait::, Distance)
JSON_DEFAULT_TRAITS(tsar::trait::, Range)
JSON_DEFAULT_TRAITS(tsar::trait::, Loop)
JSON_DEFAULT_TRAITS(tsar::trait::, Info)

#endif//ANALYSIS_JSON_H
