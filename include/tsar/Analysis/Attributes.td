//===- Attribute.td --- Defines all TSAR attributes --------*- tablegen -*-===//
//
//                      Traits Static Analyzer (SAPFOR)
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
// This file defines properties of all TSAR attributes.
//
//===----------------------------------------------------------------------===//

// Define one attribute.
class Attribute<string name> {
  string Name = name;
}

def NoIO : Attribute<"sapfor.noio">;
def AlwaysReturn : Attribute<"sapfor.alwaysreturn">;
def LibFunc : Attribute<"sapfor.libfunc">;
def DirectUserCallee : Attribute<"sapfor.direct-user-callee">;
def Inline : Attribute<"sapfor.inline">;
def IndirectCall : Attribute<"sapfor.indirect-call">;