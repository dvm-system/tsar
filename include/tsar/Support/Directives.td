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
def EK_Identifier       : ExprKind<1, "identifier">;
def EK_PPIdentifier     : ExprKind<1, "identifier">;
def EK_NumericConstant  : ExprKind<1, "numeric_constant">;
def EK_Comma            : ExprKind<1, "comma">;
def EK_Colon            : ExprKind<1, "colon">;
def EK_Arrow            : ExprKind<1, "arrow">;
def EK_LParen           : ExprKind<1, "l_paren">;
def EK_RParen           : ExprKind<1, "r_paren">;
def EK_LSquare          : ExprKind<1, "l_square">;
def EK_RSquare          : ExprKind<1, "r_square">;
def EK_LBrace           : ExprKind<1, "l_brace">;
def EK_RBrace           : ExprKind<1, "r_brace">;
def EK_Period           : ExprKind<1, "period">;
def EK_Equal            : ExprKind<1, "equal">;

// Blocks of tokens.
def EK_ZeroOrOne        : ExprKind;
def EK_ZeroOrMore       : ExprKind;
def EK_OneOrMore        : ExprKind;
def EK_OneOf            : ExprKind;
def EK_One              : ExprKind;

class Expr<ExprKind kind, list<Expr> expr_list = []> {
  ExprKind Kind = kind;
  list<Expr> ExprList = expr_list;
}

// Warning: generator automaticaly inserts anchors, do not use it manually.
def Anchor : Expr<EK_Anchor>;
def Identifier : Expr<EK_Identifier>;
def PPIdentifier : Expr<EK_PPIdentifier>;
def NumericConstant : Expr<EK_NumericConstant>;
def Comma : Expr<EK_Comma>;
def Colon : Expr<EK_Colon>;
def Arrow : Expr<EK_Arrow>;
def LParen : Expr<EK_LParen>;
def RParen : Expr<EK_RParen>;
def LSquare : Expr<EK_LSquare>;
def RSquare : Expr<EK_RSquare>;
def LBrace : Expr<EK_LBrace>;
def RBrace : Expr<EK_RBrace>;
def Period : Expr<EK_Period>;
def Equal : Expr<EK_Equal>;

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
class Directive<string name, Namespace parent = Spf, bit has_body = 0> {
  bit HasBody = has_body;
  string Name = name;
  Namespace Parent = parent;
}

def Analysis   : Directive<"analysis">;
def Transform  : Directive<"transform">;
def Assert     : Directive<"assert">;
def Region     : Directive<"region">;
def Metadata   : Directive<"metadata">;
def Parallel   : Directive<"parallel">;

def DvmTemplate     : Directive<"template", Dvm, 1>;
def DvmArray        : Directive<"array", Dvm>;
def DvmInherit      : Directive<"inherit", Dvm, 1>;
def DvmRegion       : Directive<"region", Dvm>;
def DvmHostSection  : Directive<"host_section", Dvm>;
def DvmParallel     : Directive<"parallel", Dvm, 1>;
def DvmActual       : Directive<"actual", Dvm, 1>;
def DvmGetActual    : Directive<"get_actual", Dvm, 1>;
def DvmRedistribute : Directive<"redistribute", Dvm, 1>;
def DvmRealign      : Directive<"realign", Dvm, 1>;
def DvmRemoteAccess : Directive<"remote_access", Dvm, 1>;
def DvmInterval     : Directive<"interval", Dvm, 1>;
def DvmEndInterval  : Directive<"endinterval", Dvm>;

// Define one clause.
class Clause<string name, Directive parent, list<Expr> expr_list = []> {
  string Name = name;
  Directive Parent = parent;
  list<Expr> ExprList = expr_list;
}

def ProcessPrivate : Clause<"processprivate", Parallel,
  [LParen, Identifier, ZeroOrMore<[Comma, Identifier]>, RParen]>;

def NoStrict : Clause<"nostrict", Transform>;

def Inline : Clause<"inline", Transform>;

def NoInline : Clause<"noinline", Transform>;

def Propagate : Clause<"propagate", Transform>;

def Rename : Clause<"rename", Transform>;

def RemoveUnreachable : Clause<"removeunreachable", Transform>;

def LoopInterchange : Clause<"interchange", Transform,
  [LParen, PPIdentifier, Comma, PPIdentifier, RParen]>;

def Replace : Clause<"replace", Transform,
  [ZeroOrOne<[LParen, Identifier, ZeroOrMore<[Comma, Identifier]>, RParen]>]>;

def With : Clause<"with", Transform, [LParen, Identifier, RParen]>;

def LoopReverse: Clause<"reverse", Transform,
  [LParen, OneOf<[PPIdentifier,NumericConstant]>, RParen]>;

def Private : Clause<"private", Analysis,
  [LParen, Identifier, ZeroOrMore<[Comma, Identifier]>, RParen]>;

def Induction : Clause<"induction", Analysis,
  [LParen, Identifier, ZeroOrMore<[Comma, Identifier]>, RParen]>;

def AssertNoMacro : Clause<"nomacro", Assert>;

def RegionName : Clause<"name", Region,
  [LParen, PPIdentifier, ZeroOrMore<[Comma, PPIdentifier]>, RParen]>;

def ReplaceMetadata : Clause<"replace", Metadata,
  [LParen,
    Identifier, LParen,
      ZeroOrOne<[
        OneOf<[
          One<[Identifier]>,
          One<[
            LBrace,
              ZeroOrOne<[
                ZeroOrOne<[LSquare, RSquare]>,
                  Period, PPIdentifier, Equal, Identifier,
                ZeroOrMore<[Comma, ZeroOrOne<[LSquare, RSquare]>,
                  Period, PPIdentifier, Equal, Identifier]>
              ]>,
            RBrace
          ]>
        ]>,
        ZeroOrMore<[Comma,
          OneOf<[
            One<[Identifier]>,
            One<[
              LBrace,
                ZeroOrOne<[
                  ZeroOrOne<[LSquare, RSquare]>,
                    Period, PPIdentifier, Equal, Identifier,
                  ZeroOrMore<[Comma, ZeroOrOne<[LSquare, RSquare]>,
                    Period, PPIdentifier, Equal, Identifier]>
                ]>,
              RBrace
            ]>
          ]>
        ]>
      ]>,
    RParen,
    ZeroOrMore<[Comma,
      Identifier, LParen,
        ZeroOrOne<[
          OneOf<[
            One<[Identifier]>,
            One<[
              LBrace,
                ZeroOrOne<[
                  ZeroOrOne<[LSquare, RSquare]>,
                    Period, PPIdentifier, Equal, Identifier,
                  ZeroOrMore<[Comma, ZeroOrOne<[LSquare, RSquare]>,
                    Period, PPIdentifier, Equal, Identifier]>
                ]>,
              RBrace
            ]>
          ]>,
          ZeroOrMore<[Comma,
            OneOf<[
              One<[Identifier]>,
              One<[
                LBrace,
                  ZeroOrOne<[
                    ZeroOrOne<[LSquare, RSquare]>,
                      Period, PPIdentifier, Equal, Identifier,
                    ZeroOrMore<[Comma, ZeroOrOne<[LSquare, RSquare]>,
                      Period, PPIdentifier, Equal, Identifier]>
                  ]>,
                RBrace
              ]>
            ]>
          ]>
        ]>,
      RParen
    ]>,
  RParen]>;

// DVMH clauses
def DvmTemplateBody : Clause<"", DvmTemplate>;
def DvmActualBody : Clause<"", DvmActual>;
def DvmParallelBody : Clause<"", DvmParallel>;
def DvmGetActualBody : Clause<"", DvmGetActual>;
def DvmRedistributeBody : Clause<"", DvmRedistribute>;
def DvmRealignBody : Clause<"", DvmRegion>;
def DvmRemoteAccessBody : Clause<"", DvmRemoteAccess>;
def DvmInheritBody : Clause<"", DvmInherit>;
def DvmIntervalBody : Clause<"", DvmInterval>;

def DvmArrayDistribute : Clause<"distribute", DvmArray>;
def DvmAlign : Clause<"align", DvmArray>;
def DvmShadow : Clause<"shadow", DvmArray>;

def DvmTemplateDistribute : Clause<"distribute", DvmTemplate>;

def DvmParallelPrivate : Clause<"private", DvmParallel>;
def DvmParallelReduction : Clause<"reduction", DvmParallel>;
def DvmParallelRemoteAccess : Clause<"remote_access", DvmParallel>;
def DvmParallelShadowRenew : Clause<"shadow_renew", DvmParallel>;
def DvmParallelAcross : Clause<"accross", DvmParallel>;
def DvmParallelStage : Clause<"staget", DvmParallel>;
def DvmParallelCudaBlock : Clause<"cuda_block", DvmParallel>;

def DvmRegionIn : Clause<"in", DvmRegion>;
def DvmRegionOut : Clause<"out", DvmRegion>;
def DvmRegionLocal : Clause<"local", DvmRegion>;
def DvmRegionInOut : Clause<"inout", DvmRegion>;
def DvmRegionInLocal : Clause<"inlocal", DvmRegion>;
def DvmRegionTarget : Clause<"target", DvmRegion>;
def DvmRegionAsync : Clause<"async", DvmRegion>;
