#include "tsar/Analysis/PDG.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Core/Query.h"
#include <llvm/Support/GenericDomTreeConstruction.h>
#include <llvm/Support/GraphWriter.h>
#include <set>
#include <string>

using namespace tsar;
using namespace llvm;
using namespace clang;
using namespace std;

template<typename KeyT, typename ValueT, typename Container>
void addToMap(map<KeyT, Container> &Map, KeyT Key, ValueT Value) {
	auto It=Map.find(Key);
	if (It!=Map.end())
		Map[Key].insert(Value);
	else
		Map.insert({Key, {Value}});
}

inline void PDGBuilder::processControlDependence() {
	map<SourceCFGNode*, set<SourceCFGNode*>> DependenceInfo;
	for (auto SourceNodeIt=++df_begin(getRealRoot());
    SourceNodeIt!=df_end(getRealRoot()); ++SourceNodeIt)
		for (auto TargetNodeIt=++df_begin(SourceNodeIt->getIDom());
      TargetNodeIt!=df_end(SourceNodeIt->getIDom()); ++TargetNodeIt)
			if (SourceNodeIt->getBlock()->hasEdgeTo(*TargetNodeIt->getBlock())
				/*
        && !( (SourceNodeIt->getBlock()->getKind()==
        SourceCFGNode::NodeKind::Service
				&& ((ServiceNode*)SourceNodeIt->getBlock())->getType()!=
        ServiceNode::NodeType::GraphEntry)
				|| (TargetNodeIt->getBlock()->getKind()==
        SourceCFGNode::NodeKind::Service
				&& ((ServiceNode*)TargetNodeIt->getBlock())->getType()!=
        ServiceNode::NodeType::GraphEntry) )
        */
        )
				for (unsigned I=TargetNodeIt.getPathLength()-1; I>0; --I)
					addToMap(DependenceInfo, SourceNodeIt->getBlock(),
              TargetNodeIt.getPath(I)->getBlock());
	for (auto N : *mSCFG)
		if (!(N->getKind()==SourceCFGNode::NodeKind::Service &&
      ((ServiceSCFGNode*)N)->getType()!=ServiceSCFGNode::NodeType::GraphEntry))
			mPDG->emplaceNode(N);
	for (auto DIIt : DependenceInfo)
		for (auto TargetNodeIt : DIIt.second) {
			if (DIIt.first->getKind()==SourceCFGNode::NodeKind::Service
				&& ((ServiceSCFGNode*)DIIt.first)->getType()!=
          ServiceSCFGNode::NodeType::GraphEntry)
				break;
			if (!(TargetNodeIt->getKind()==SourceCFGNode::NodeKind::Service
				&& ((ServiceSCFGNode*)TargetNodeIt)->getType()!=
        ServiceSCFGNode::NodeType::GraphEntry))
				mPDG->bindNodes(*mPDG->getNode(DIIt.first),
            *mPDG->getNode(TargetNodeIt),
            PDGEdge::EdgeKind::ControlDependence);
		}
}

PDG *PDGBuilder::populate(SourceCFG &_SCFG) {
	ServiceSCFGNode *EntryNode;
	if (!_SCFG.getStopNode())
		return nullptr;
	mPDG=new tsar::PDG(string(_SCFG.getName()), &_SCFG);
	mSCFG=&_SCFG;
	mSCFG->emplaceEntryNode();
	mSCFG->recalculatePredMap();
  mSPDT=PostDomTreeBase<SourceCFGNode>();
  mSPDT.recalculate(*mSCFG);
	/*May be useful
  dumpDotGraphToFile(mSPDT, "post-dom-tree.dot", "post-dom-tree");
  */
	processControlDependence();
	return mPDG;
}

char PDGPass::ID=0;

INITIALIZE_PASS_BEGIN(PDGPass, "pdg",
	"Program Dependency Graph", false, true)
INITIALIZE_PASS_DEPENDENCY(ClangSourceCFGPass)
INITIALIZE_PASS_END(PDGPass, "pdg",
	"Program Dependency Graph", false, true)

FunctionPass *createPDGPass() { return new PDGPass; }

void PDGPass::getAnalysisUsage(AnalysisUsage &AU) const {
	AU.addRequired<ClangSourceCFGPass>();
	AU.setPreservesAll();    
}

bool PDGPass::runOnFunction(Function &F) {
	releaseMemory();
	auto &SCFGPass=getAnalysis<ClangSourceCFGPass>();
	mPDG=mPDGBuilder.populate(SCFGPass.getSourceCFG());
	return false;
}

namespace {
struct PDGPassGraphTraits {
	static tsar::PDG *getGraph(PDGPass *P) { return &P->getPDG(); }
};

struct PDGPrinter : public DOTGraphTraitsPrinterWrapperPass<
	PDGPass, false, tsar::PDG*,
	PDGPassGraphTraits> {
	static char ID;
	PDGPrinter() : DOTGraphTraitsPrinterWrapperPass<PDGPass, false,
		tsar::PDG*, PDGPassGraphTraits>("pdg", ID) {
		initializePDGPrinterPass(*PassRegistry::getPassRegistry());
	}
};
char PDGPrinter::ID = 0;

struct PDGViewer : public DOTGraphTraitsViewerWrapperPass<
	PDGPass, false, tsar::PDG*,
	PDGPassGraphTraits> {
	static char ID;
	PDGViewer() : DOTGraphTraitsViewerWrapperPass<PDGPass, false,
		tsar::PDG*, PDGPassGraphTraits>("pdg", ID) {
		initializePDGViewerPass(*PassRegistry::getPassRegistry());
	}
};
char PDGViewer::ID = 0;
} //anonymous namespace

INITIALIZE_PASS_IN_GROUP(PDGViewer, "view-pdg",
	"View Program Dependency Graph", true, true,
	DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(PDGPrinter, "print-pdg",
	"Print Program Dependency Graph", true, true,
	DefaultQueryManager::OutputPassGroup::getPassRegistry())

FunctionPass *llvm::createPDGPrinter() {
  return new PDGPrinter;
}

FunctionPass *llvm::createPDGViewer() {
  return new PDGViewer;
}
