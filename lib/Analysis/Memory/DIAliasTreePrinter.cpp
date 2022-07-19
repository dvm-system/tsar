//===- DIAliasTreePrinter.cpp - Memory Hierarchy Printer (Debug) *- C++ -*-===//
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
//
//===----------------------------------------------------------------------===//
//
// This file implements a 'dot-di-em' analysis pass, which emits the
// em.<fnname>.dot file for each function in the program in the program,
// with an alias tree for that function. This file also implements a 'view-di-em'
// analysis pass which display this graph.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Core/Query.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/Analysis/DOTGraphTraitsPass.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/Support/GraphWriter.h>

using namespace llvm;
using namespace tsar;

namespace llvm {
/// TODO (kaniandr@gmail.com): it seems there is a bug in a new LLVM version,
/// so mix of GraphT and GraphT * is used as a parameter fro DOTRgraphTraits.
template <> struct DOTGraphTraits<DIAliasTree**> {
  static std::string getGraphName(DIAliasTree **) {
    return "Alias Tree (Debug)";
  }
};

template<> struct DOTGraphTraits<DIAliasTree *> :
  public DefaultDOTGraphTraits {

  using GT = GraphTraits<DIAliasTree *>;
  using EdgeItr = typename GT::ChildIteratorType;

  explicit DOTGraphTraits(bool IsSimple = false) :
    DefaultDOTGraphTraits(IsSimple) {}

  static std::string getGraphName(const DIAliasTree */*G*/) {
    return "Alias Tree (Debug)";
  }

  std::string getNodeLabel(DIAliasNode *N, DIAliasTree *G) {
    std::string Str;
    llvm::raw_string_ostream OS(Str);
    switch (N->getKind()) {
    default:
      llvm_unreachable("Unknown kind of an alias node!");
      break;
    case DIAliasNode::KIND_TOP:  return "Whole Memory";
    case DIAliasNode::KIND_ESTIMATE: break;
    case DIAliasNode::KIND_UNKNOWN: OS << "Unknown Memory\n"; break;
    }
    auto DWLang = getLanguage(G->getFunction());
    if (!DWLang) {
      OS << "Unknown Source Language" << "\n";
      return OS.str();
    }
    for (auto &M : cast<DIAliasMemoryNode>(*N)) {
      printDILocationSource(*DWLang, M, OS);
      OS << (!M.isExplicit() ? "*" : "") << ' ';
    }
    return OS.str();
  }

  static std::string getEdgeAttributes(
    DIAliasNode *N, EdgeItr /*E*/, const DIAliasTree */*G*/) {
    if (isa<DIAliasUnknownNode>(N))
      return "style=dashed";
    return "";
  }
};

struct DIEstimateMemoryPassGraphTraits {
  static DIAliasTree * getGraph(DIEstimateMemoryPass *EMP) {
    return &EMP->getAliasTree();
  }
};
}

namespace {
struct DIAliasTreePrinter :
    public DOTGraphTraitsPrinterWrapperPass<DIEstimateMemoryPass,
      false, tsar::DIAliasTree *, DIEstimateMemoryPassGraphTraits> {
  static char ID;
  DIAliasTreePrinter() :
      DOTGraphTraitsPrinterWrapperPass<DIEstimateMemoryPass, false,
        tsar::DIAliasTree *, DIEstimateMemoryPassGraphTraits>("em-di", ID) {
    initializeDIAliasTreePrinterPass(*PassRegistry::getPassRegistry());
  }
};

char DIAliasTreePrinter::ID = 0;

struct DIAliasTreeViewer :
  public DOTGraphTraitsViewerWrapperPass<DIEstimateMemoryPass,
    false, tsar::DIAliasTree *, DIEstimateMemoryPassGraphTraits> {
  static char ID;
  DIAliasTreeViewer() :
      DOTGraphTraitsViewerWrapperPass<DIEstimateMemoryPass, false,
        tsar::DIAliasTree *, DIEstimateMemoryPassGraphTraits>("em-di", ID) {
    initializeDIAliasTreeViewerPass(*PassRegistry::getPassRegistry());
  }
};

char DIAliasTreeViewer::ID = 0;
}

INITIALIZE_PASS_IN_GROUP(DIAliasTreeViewer, "view-em-di",
  "View metadata-level alias tree of a function", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(DIAliasTreePrinter, "dot-em-di",
  "Print metadata-level alias tree to 'dot' file", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

FunctionPass * llvm::createDIAliasTreeViewerPass() {
  return new DIAliasTreeViewer();
}

FunctionPass * llvm::createDIAliasTreePrinterPass() {
  return new DIAliasTreePrinter();
}

void DIAliasTree::view() const {
  llvm::ViewGraph(const_cast<DIAliasTree *>(this), "em-di", true,
    llvm::DOTGraphTraits<DIAliasTree *>::getGraphName(this));
}
