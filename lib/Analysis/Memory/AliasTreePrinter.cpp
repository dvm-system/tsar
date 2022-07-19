//===-- AliasTreePrinter.cpp - Memory Hierarchy Printer ---------*- C++ -*-===//
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
// This file implements a 'dot-em' analysis pass, which emits the
// em.<fnname>.dot file for each function in the program in the program,
// with an alias tree for that function. This file also implements a 'view-em'
// analysis pass which display this graph.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Core/Query.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/Analysis/DOTGraphTraitsPass.h>
#include <llvm/Support/GraphWriter.h>

using namespace llvm;
using namespace tsar;

namespace llvm {
/// TODO (kaniandr@gmail.com): it seems there is a bug in a new LLVM version,
/// so mix of GraphT and GraphT * is used as a parameter fro DOTRgraphTraits.
template <> struct DOTGraphTraits<AliasTree**> {
  static std::string getGraphName(AliasTree **) {
    return "Alias Tree";
  }
};

template<> struct DOTGraphTraits<AliasTree *> :
    public DefaultDOTGraphTraits {

  using GT = GraphTraits<AliasTree *>;
  using EdgeItr = typename GT::ChildIteratorType;

  explicit DOTGraphTraits(bool IsSimple = false) :
    DefaultDOTGraphTraits(IsSimple) {}

  static std::string getGraphName(const AliasTree */*G*/) {
    return "Alias Tree";
  }

  std::string getNodeLabel(AliasTopNode */*N*/, AliasTree */*G*/) {
      return "Whole Memory";
  }

  std::string getNodeLabel(AliasEstimateNode *N, AliasTree *G) {
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    for (auto &EM : *N) {
      if (isSimple()) {
        MemoryLocation Loc(EM.front(), EM.getSize(), EM.getAAInfo());
        printLocationSource(OS, Loc, &G->getDomTree());
        OS << (!EM.isExplicit() ? "*" : "") << ' ';
      } else if (EM.isAmbiguous()) {
        OS << "Ambiguous, size " << EM.getSize();
        OS << (EM.isExplicit() ? ", explicit" : ", implicit");
        OS << "\\l";
        for (auto Ptr : EM) {
          OS << "  ";
          if (isa<Function>(Ptr))
            Ptr->printAsOperand(OS);
          else
            Ptr->print(OS, true);
          OS << "\\l";
        }
      } else {
        if (isa<Function>(EM.front()))
          EM.front()->printAsOperand(OS);
        else
          EM.front()->print(OS, true);
        OS << ", size " << EM.getSize();
        OS << (EM.isExplicit() ? ", explicit" : ", implicit");
        OS << "\\l";
      }
    }
    return OS.str();
  }

  std::string getNodeLabel(AliasUnknownNode *N, AliasTree */*G*/) {
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    OS << "Unknown Memory\n";
    for (auto *Unknown : *N) {
      if (isSimple()) {
        if (auto Callee = [Unknown]() -> Function * {
              if (auto *Call = dyn_cast<CallBase>(Unknown))
                return dyn_cast<Function>(
                    Call->getCalledOperand()->stripPointerCasts());
              return nullptr;
            }())
          Callee->printAsOperand(OS, false);
        else
          Unknown->printAsOperand(OS, false);
        OS << ' ';
      } else if (isa<Function>(*Unknown)) {
        Unknown->printAsOperand(OS, true);
        OS << "\\l";
      }
      else {
        Unknown->print(OS, true);
        OS << "\\l";
      }
    }
   return OS.str();
  }

  std::string getNodeLabel(AliasNode *N, AliasTree *G) {
    switch (N->getKind()) {
    default:
      llvm_unreachable("Unknown kind of an alias node!");
      break;
    case AliasNode::KIND_TOP:
      return getNodeLabel(cast<AliasTopNode>(N), G);
    case AliasNode::KIND_ESTIMATE:
      return getNodeLabel(cast<AliasEstimateNode>(N), G);
    case AliasNode::KIND_UNKNOWN:
      return getNodeLabel(cast<AliasUnknownNode>(N), G);
    }
  }

  static std::string getEdgeAttributes(
    AliasNode *N, EdgeItr /*E*/, const AliasTree */*G*/) {
    if (isa<AliasUnknownNode>(N))
      return "style=dashed";
    return "";
  }
};

struct EstimateMemoryPassGraphTraits {
  static AliasTree * getGraph(EstimateMemoryPass *EMP) {
    return &EMP->getAliasTree();
  }
};
}

namespace {
struct AliasTreePrinter:
  public DOTGraphTraitsPrinterWrapperPass<EstimateMemoryPass,
    false, tsar::AliasTree *, EstimateMemoryPassGraphTraits> {
  static char ID;
  AliasTreePrinter() :
    DOTGraphTraitsPrinterWrapperPass<EstimateMemoryPass,
      false, tsar::AliasTree *, EstimateMemoryPassGraphTraits>("em", ID) {
    initializeAliasTreePrinterPass(*PassRegistry::getPassRegistry());
  }
};

char AliasTreePrinter::ID = 0;

struct AliasTreeOnlyPrinter :
  public DOTGraphTraitsPrinterWrapperPass<EstimateMemoryPass,
    true, tsar::AliasTree *, EstimateMemoryPassGraphTraits> {
  static char ID;
  AliasTreeOnlyPrinter() :
    DOTGraphTraitsPrinterWrapperPass<EstimateMemoryPass,
      true, tsar::AliasTree *, EstimateMemoryPassGraphTraits>("emonly", ID) {
    initializeAliasTreeOnlyPrinterPass(*PassRegistry::getPassRegistry());
  }
};

char AliasTreeOnlyPrinter::ID = 0;

struct AliasTreeViewer :
  public DOTGraphTraitsViewerWrapperPass<EstimateMemoryPass,
    false, tsar::AliasTree *, EstimateMemoryPassGraphTraits> {
  static char ID;
  AliasTreeViewer() :
    DOTGraphTraitsViewerWrapperPass<EstimateMemoryPass,
      false, tsar::AliasTree *, EstimateMemoryPassGraphTraits>("em", ID) {
    initializeAliasTreeViewerPass(*PassRegistry::getPassRegistry());
  }
};

char AliasTreeViewer::ID = 0;

struct AliasTreeOnlyViewer :
  public DOTGraphTraitsViewerWrapperPass<EstimateMemoryPass,
    true, tsar::AliasTree *, EstimateMemoryPassGraphTraits> {
  static char ID;
  AliasTreeOnlyViewer() :
    DOTGraphTraitsViewerWrapperPass<EstimateMemoryPass,
      true, tsar::AliasTree *, EstimateMemoryPassGraphTraits>("emonly", ID) {
    initializeAliasTreeOnlyViewerPass(*PassRegistry::getPassRegistry());
  }
};

char AliasTreeOnlyViewer::ID = 0;
}

INITIALIZE_PASS_IN_GROUP(AliasTreeViewer, "view-em",
  "View alias tree of a function", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(AliasTreeOnlyViewer, "view-em-only",
  "View alias tree of a function (alias summary only)", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(AliasTreePrinter, "dot-em",
  "Print alias tree to 'dot' file", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(AliasTreeOnlyPrinter, "dot-em-only",
  "Print alias tree to 'dot' file (alias summary only)", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

FunctionPass * llvm::createAliasTreeViewerPass() {
  return new AliasTreeViewer();
}

FunctionPass * llvm::createAliasTreeOnlyViewerPass() {
  return new AliasTreeOnlyViewer();
}

FunctionPass * llvm::createAliasTreePrinterPass() {
  return new AliasTreePrinter();
}

FunctionPass * llvm::createAliasTreeOnlyPrinterPass() {
  return new AliasTreeOnlyPrinter();
}

void AliasTree::view() const {
  llvm::ViewGraph(const_cast<AliasTree *>(this), "em", false,
    llvm::DOTGraphTraits<AliasTree *>::getGraphName(this));
}

void AliasTree::viewOnly() const {
  llvm::ViewGraph(const_cast<AliasTree *>(this), "emonly", true,
    llvm::DOTGraphTraits<AliasTree *>::getGraphName(this));
}
