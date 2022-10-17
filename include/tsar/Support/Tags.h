//===----- Tags.h --------------- List Of Tags ------------------*- C++ -*-===//
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
// This file defines tags to identify mark different objects.
//
//===----------------------------------------------------------------------===//

#ifndef TSAR_SUPPORT_TAGS_H
#define TSAR_SUPPORT_TAGS_H

namespace llvm {
class MDNode;
}

namespace tsar {
/// This tag provides access to low-level representation of matched entities.
struct IR {};

/// This tag provides access to source-level representation of matched entities.
struct AST {};

/// This tag provides access to metadata-level representation of matched entities.
struct MD {};

/// This tag is used to implement hierarchy of nodes.
struct Hierarchy {};

/// This tag is used to implement a sequence of memory locations which may alias.
struct Alias {};

/// This tag is used to implement a sequence of sibling nodes.
struct Sibling {};

/// This tag is used to implement a sequence of nodes which is treated as a pool.
struct Pool {};

/// This tag is used to provide access to a set of traits of a region.
struct Region {};

/// This tag is used to provide access to entities related to a original object.
struct Origin {};

/// This tag is used to provide access to entities related to a cloned object.
struct Clone {};

/// This tag is used to represent the beginning of a range.
struct Begin {};

/// This tag is used to represent a range end.
struct End {};

/// This tag is used to represent step in a sequence of values.
struct Step {};

/// This tag is used to provide access to a root object.
struct Root {};

/// This tag is used to provide access to a description of a file.
struct File {};

/// This tag is used to provide access to a description of a line.
struct Line {};

/// This tag is used to provide access to a description of a column.
struct Column {};

/// This tag is used to provide access to a description of an identifier.
struct Identifier {};

/// Identifier of an object, for example, loops.
using ObjectID = llvm::MDNode *;
}
#endif//TSAR_SUPPORT_TAGS_H
