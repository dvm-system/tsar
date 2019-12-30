//===---- Directives.td - Defines all TSAR directives ------*- tablegen -*-===//
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
// This file defines properties of all TSAR directives and clauses.
//
//===----------------------------------------------------------------------===//

class ExprKind<bit is_single = 0, string clang_tok="unknown"> {
  bit IsSingle = is_single;
  string ClangTok = clang_tok;
}

def EK_Anchor       : ExprKind<1>;

// Single tokens.
def EK_Identifier   : ExprKind<1, "identifier">;
def EK_PPIdentifier : ExprKind<1, "identifier">;
def EK_Comma        : ExprKind<1, "comma">;
def EK_LParen       : ExprKind<1, "l_paren">;
def EK_RParen       : ExprKind<1, "r_paren">;

// Blocks of tokens.
def EK_ZeroOrOne    : ExprKind;
def EK_ZeroOrMore   : ExprKind;
def EK_OneOrMore    : ExprKind;
def EK_OneOf        : ExprKind;
def EK_One          : ExprKind;

class Expr<ExprKind kind, list<Expr> expr_list = []> {
  ExprKind Kind = kind;
  list<Expr> ExprList = expr_list;
}

// Warning: generator automaticaly inserts anchors, do not use it manually.
def Anchor : Expr<EK_Anchor>;
def Identifier : Expr<EK_Identifier>;
def PPIdentifier : Expr<EK_PPIdentifier>;
def Comma : Expr<EK_Comma>;
def LParen : Expr<EK_LParen>;
def RParen : Expr<EK_RParen>;

class ZeroOrOne<list<Expr> expr_list = []> : Expr<EK_ZeroOrOne, expr_list> {}
class ZeroOrMore<list<Expr> expr_list = []> : Expr<EK_ZeroOrMore, expr_list> {}
class OneOrMore<list<Expr> expr_list = []> : Expr<EK_OneOrMore, expr_list> {}
class OneOf<list<Expr> expr_list = []> : Expr<EK_OneOf, expr_list> {}
class One<list<Expr> expr_list = []> : Expr<EK_One, expr_list> {}

// Define namespace which contain directives.
class Namespace<string name> {
  string Name = name;
}

def Spf : Namespace<"spf">;
def Dvm : Namespace<"dvm">;

// Define one directive which may contain multiple clauses.
class Directive<string name, Namespace parent = Spf> {
  string Name = name;
  Namespace Parent = parent;
}

def Analysis   : Directive<"analysis">;
def Transform  : Directive<"transform">;
def Assert     : Directive<"assert">;
def Region     : Directive<"region">;

def DvmTemplate: Directive<"template", Dvm>;
def DvmArray   : Directive<"array", Dvm>;
def DvmInherit : Directive<"inherit", Dvm>;

// Define one clause.
class Clause<string name, Directive parent, list<Expr> expr_list = []> {
  string Name = name;
  Directive Parent = parent;
  list<Expr> ExprList = expr_list;
}

def Inline : Clause<"inline", Transform>;

def NoInline : Clause<"noinline", Transform>;

def Propagate : Clause<"propagate", Transform>;

def Rename : Clause<"rename", Transform>;

def Private : Clause<"private", Analysis,
  [LParen, Identifier, ZeroOrMore<[Comma, Identifier]>, RParen]>;

def Induction : Clause<"induction", Analysis,
  [LParen, Identifier, ZeroOrMore<[Comma, Identifier]>, RParen]>;

def AssertNoMacro : Clause<"nomacro", Assert>;

def RegionName : Clause<"name", Region,
  [LParen, PPIdentifier, ZeroOrMore<[Comma, PPIdentifier]>, RParen]>;

def DvmAlign : Clause<"align", DvmArray>;
