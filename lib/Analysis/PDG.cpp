#include "tsar/Analysis/PDG.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Core/Query.h"
#include <llvm/ADT/iterator.h>
#include <llvm/IR/Dominators.h>
#include <llvm/Analysis/CFGPrinter.h>
#include <llvm/Analysis/DomPrinter.h>
#include <llvm/Support/GenericDomTreeConstruction.h>
#include <llvm/Support/GraphWriter.h>
#include <set>
#include <string>

//
#include "tsar/Analysis/Memory/DependenceAnalysis.h"
#include <llvm/InitializePasses.h>
//
#include <llvm/Support/CommandLine.h>

using namespace tsar;
using namespace llvm;
using namespace clang;
using namespace std;

namespace llvm {
template<>
struct DOTGraphTraits<Function*>
		: public DOTGraphTraits<DOTFuncInfo*> {
	DOTGraphTraits(bool isSimple=false) : DOTGraphTraits<DOTFuncInfo*>(isSimple) {}
	string getNodeLabel(const BasicBlock *Node, Function*) {
		return DOTGraphTraits<DOTFuncInfo*>::getNodeLabel(Node, nullptr);
	}
	string getNodeAttributes(const BasicBlock *Node, Function *F) {
		DOTFuncInfo DOTInfo(F);
		return DOTGraphTraits<DOTFuncInfo*>::getNodeAttributes(Node, &DOTInfo);
	}
	string getEdgeAttributes(const BasicBlock *Node,
			const_succ_iterator EdgeIt, Function *F) {
		DOTFuncInfo DOTInfo(F);
		return DOTGraphTraits<DOTFuncInfo*>::getEdgeAttributes(Node, EdgeIt, &DOTInfo);
	}
	bool isNodeHidden(const BasicBlock *Node, const Function *F) {
		DOTFuncInfo DOTInfo(F);
		return DOTGraphTraits<DOTFuncInfo*>::isNodeHidden(Node, &DOTInfo);
	}
};

template<> struct GraphTraits<DomTreeNodeBase<tsar::SourceCFGNode>*>
		: public DomTreeGraphTraitsBase<DomTreeNodeBase<tsar::SourceCFGNode>,
		DomTreeNodeBase<tsar::SourceCFGNode>::const_iterator> {};

template<> struct GraphTraits<const DomTreeNodeBase<tsar::SourceCFGNode>*>
		: public DomTreeGraphTraitsBase<const DomTreeNodeBase<tsar::SourceCFGNode>,
		DomTreeNodeBase<tsar::SourceCFGNode>::const_iterator> {};

template<typename NodeValueType>
struct GraphTraits<PostDomTreeBase<NodeValueType>*>
		: public GraphTraits<DomTreeNodeBase<NodeValueType>*> {
private:
	using BaseGT=GraphTraits<DomTreeNodeBase<NodeValueType>*>;
public:
	static typename BaseGT::NodeRef getEntryNode(PostDomTreeBase<NodeValueType> *PDT) {
		PDT->getRootNode();
	}
	static typename BaseGT::nodes_iterator nodes_begin(PostDomTreeBase<NodeValueType> *PDT) {
		return df_begin(getEntryNode(PDT));
	}
	static typename BaseGT::nodes_iterator nodes_end(PostDomTreeBase<NodeValueType> *PDT) {
		return df_end(getEntryNode(PDT));
	}
};

template<typename NodeValueType>
struct DOTGraphTraits<PostDomTreeBase<NodeValueType>*>
		: public DOTGraphTraits<typename PostDomTreeBase<NodeValueType>::ParentPtr> {
private:
	using BaseDOTGT=DOTGraphTraits<typename PostDomTreeBase<NodeValueType>::ParentPtr>;
public:
	DOTGraphTraits(bool IsSimple=false) : BaseDOTGT(IsSimple) {}
	static std::string getGraphName(const PostDomTreeBase<NodeValueType> *Tree) {
		return "Post-Dominator Tree";
	}
	std::string getNodeLabel(DomTreeNodeBase<NodeValueType> *Node,
			PostDomTreeBase<NodeValueType> *Tree) {
		if (Tree->isVirtualRoot(Node))
			return "Virtual Root";
		else
			return BaseDOTGT::getNodeLabel(Node->getBlock(), Node->getBlock()->getParent());
			
	}
	std::string getEdgeSourceLabel(DomTreeNodeBase<NodeValueType> *Node,
			typename GraphTraits<PostDomTreeBase<NodeValueType>*>::ChildIteratorType It) { return ""; }
	static bool isNodeHidden(DomTreeNodeBase<NodeValueType> *Node,
			PostDomTreeBase<NodeValueType> *Tree) {
		return false;
	}
	std::string getNodeAttributes(DomTreeNodeBase<NodeValueType> *Node,
			PostDomTreeBase<NodeValueType> *Tree) {
		if (Tree->isVirtualRoot(Node))
			return "";
		else
			return BaseDOTGT::getNodeAttributes(Node->getBlock(), Node->getBlock()->getParent());
	}
	std::string getEdgeAttributes(DomTreeNodeBase<NodeValueType>*,
			typename GraphTraits<PostDomTreeBase<NodeValueType>*>::ChildIteratorType,
			PostDomTreeBase<NodeValueType>*) {
		return "";
	}
};
}; //namespace llvm

namespace {
template<typename KeyT, typename ValueT, typename Container>
void addToMap(map<KeyT, Container> &Map, KeyT Key, ValueT Value) {
	auto It=Map.find(Key);
	if (It!=Map.end())
		Map[Key].insert(Value);
	else
		Map.insert({Key, {Value}});
}

template<typename NodeType>
bool hasEdgesTo(NodeType *SourceN, NodeType *TargetN) {
	for (auto ChildNodeIt=GraphTraits<NodeType*>::child_begin(SourceN);
		ChildNodeIt!=GraphTraits<NodeType*>::child_end(SourceN); ++ChildNodeIt)
		if (*ChildNodeIt==TargetN)
			return true;
	return false;
}
}; //anonymous namespace

template<typename CDGType>
inline void CDGBuilder<CDGType>::processControlDependence() {
	map<typename CDGType::NodeType*, set<NodeValueType>> DependenceInfo;
	for (auto NIt=GraphTraits<CFGType*>::nodes_begin(mCFG); NIt!=GraphTraits<CFGType*>::nodes_end(mCFG); ++NIt)
		mCDG->emplaceNode(*NIt);
	for (auto SourceNodeIt=++df_begin(mPDT.getRootNode()); SourceNodeIt!=df_end(mPDT.getRootNode()); ++SourceNodeIt) {
		if (SourceNodeIt->getBlock()==GraphTraits<CFGType*>::getEntryNode(mCFG))
			for (unsigned I=SourceNodeIt.getPathLength()-1; I>0; --I)
				addToMap(DependenceInfo, mCDG->getEntryNode(), SourceNodeIt.getPath(I)->getBlock());
		for (auto TargetNodeIt=++df_begin(SourceNodeIt->getIDom()); TargetNodeIt!=df_end(SourceNodeIt->getIDom()); ++TargetNodeIt)
			if (hasEdgesTo(SourceNodeIt->getBlock(), TargetNodeIt->getBlock()))
				for (unsigned I=TargetNodeIt.getPathLength()-1; I>0; --I)
					addToMap(DependenceInfo, mCDG->getNode(SourceNodeIt->getBlock()), TargetNodeIt.getPath(I)->getBlock());
	}
	for (auto DIIt : DependenceInfo)
		for (auto TargetNodeIt : DIIt.second)
			mCDG->bindNodes(*DIIt.first, *mCDG->getNode(TargetNodeIt));
}

template<typename CDGType>
CDGType *CDGBuilder<CDGType>::populate(CFGType &CFG) {
	mCFG=&CFG;
	mCDG=new CDGType(string(CFG.getName()), &CFG);
	mPDT=PostDomTreeBase<CFGNodeType>();
	mPDT.recalculate(*mCFG);
	/*May be useful*/
	dumpDotGraphToFile(&mPDT, "post-dom-tree.dot", "post-dom-tree");
	/**/
	processControlDependence();
	return mCDG;
}

template<> char SourceCDGPass::ID=0;

INITIALIZE_PASS_BEGIN(SourceCDGPass, "source-cdg",
	"Control Dependence Graph from source code", false, true)
INITIALIZE_PASS_DEPENDENCY(ClangSourceCFGPass)
INITIALIZE_PASS_END(SourceCDGPass, "source-cdg",
	"Control Dependence Graph from source code", false, true)

FunctionPass *createSourceCDGPass() { return new SourceCDGPass; }

template<>
void SourceCDGPass::getAnalysisUsage(AnalysisUsage &AU) const {
	AU.addRequired<ClangSourceCFGPass>();
	AU.setPreservesAll();    
}

template<>
bool SourceCDGPass::runOnFunction(Function &F) {
	releaseMemory();
	ClangSourceCFGPass &SCFGPass=getAnalysis<ClangSourceCFGPass>();
	SCFGPass.getSourceCFG().recalculatePredMap();
	mCDG=mCDGBuilder.populate(SCFGPass.getSourceCFG());
	return false;
}

template<> char IRCDGPass::ID=0;

INITIALIZE_PASS_BEGIN(IRCDGPass, "llvm-ir-cdg",
	"Control Dependence Graph from IR", false, true)
INITIALIZE_PASS_DEPENDENCY(DependenceAnalysisWrapperPass)
INITIALIZE_PASS_DEPENDENCY(EstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(DIEstimateMemoryPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(IRCDGPass, "llvm-ir-cdg",
	"Control Dependence Graph from IR", false, true)

FunctionPass *createIRCDGPass() { return new IRCDGPass; }

template<>
void IRCDGPass::getAnalysisUsage(AnalysisUsage &AU) const {
	AU.addRequired<DependenceAnalysisWrapperPass>();
	AU.addRequired<EstimateMemoryPass>();
	AU.addRequired<DIEstimateMemoryPass>();
	AU.addRequired<TargetLibraryInfoWrapperPass>();
	AU.setPreservesAll();    
}

template<>
bool IRCDGPass::runOnFunction(Function &F) {
	releaseMemory();
	mCDG=mCDGBuilder.populate(F);
	{
		auto &DIPass=getAnalysis<DependenceAnalysisWrapperPass>();
		/*May be useful*/
		StringMap<cl::Option*> &OpMap=cl::getRegisteredOptions();
		bool OldDDGSimplifyOpt=((cl::opt<bool>*)OpMap["ddg-simplify"])->getValue(),
			OldDDGPiBlocksOpt=((cl::opt<bool>*)OpMap["ddg-pi-blocks"])->getValue();
		((cl::opt<bool>*)OpMap["ddg-simplify"])->setValue(false);
		((cl::opt<bool>*)OpMap["ddg-pi-blocks"])->setValue(false);
		//
		auto &EMPass=getAnalysis<EstimateMemoryPass>();
		auto &DIEMPass=getAnalysis<DIEstimateMemoryPass>();
		auto &mTLIPass=getAnalysis<TargetLibraryInfoWrapperPass>();
		//
		ProgramDependencyGraph PDG(*this, F, DIPass.getDI(), EMPass.getAliasTree(), DIEMPass.getAliasTree(), mTLIPass.getTLI(F));
		dumpDotGraphToFile((const ProgramDependencyGraph*)&PDG, "pdg.dot", "program dependency graph");
		DataDependenceGraph DDG(F, DIPass.getDI());
		dumpDotGraphToFile((const DataDependenceGraph*)&DDG, "data-dependence-graph.dot", "data dependence graph", false);
		((cl::opt<bool>*)OpMap["ddg-simplify"])->setValue(OldDDGSimplifyOpt);
		((cl::opt<bool>*)OpMap["ddg-pi-blocks"])->setValue(OldDDGPiBlocksOpt);
		DOTFuncInfo DOTCFGInfo(&F);
		dumpDotGraphToFile(&DOTCFGInfo, "ircfg.dot", "control flow graph");
	}
	/**/
	return false;
}

namespace {
template<typename CDGType>
struct CDGPassGraphTraits {
	static CDGType *getGraph(CDGPass<CDGType> *P) { return &P->getCDG(); }
};

struct SourceCDGPrinter : public DOTGraphTraitsPrinterWrapperPass<
	SourceCDGPass, false, ControlDependenceGraph<SourceCFG, SourceCFGNode>*,
	CDGPassGraphTraits<ControlDependenceGraph<SourceCFG, SourceCFGNode>>> {
	static char ID;
	SourceCDGPrinter() : DOTGraphTraitsPrinterWrapperPass<SourceCDGPass, false,
		ControlDependenceGraph<SourceCFG, SourceCFGNode>*,
		CDGPassGraphTraits<ControlDependenceGraph<SourceCFG, SourceCFGNode>>>("source-cdg", ID) {
		initializeSourceCDGPrinterPass(*PassRegistry::getPassRegistry());
	}
};
char SourceCDGPrinter::ID = 0;

struct SourceCDGViewer : public DOTGraphTraitsViewerWrapperPass<
	SourceCDGPass, false, ControlDependenceGraph<SourceCFG, SourceCFGNode>*,
	CDGPassGraphTraits<ControlDependenceGraph<SourceCFG, SourceCFGNode>>> {
	static char ID;
	SourceCDGViewer() : DOTGraphTraitsViewerWrapperPass<SourceCDGPass, false,
		ControlDependenceGraph<SourceCFG, SourceCFGNode>*,
		CDGPassGraphTraits<ControlDependenceGraph<SourceCFG, SourceCFGNode>>>("source-cdg", ID) {
		initializeSourceCDGViewerPass(*PassRegistry::getPassRegistry());
	}
};
char SourceCDGViewer::ID = 0;

struct IRCDGPrinter : public DOTGraphTraitsPrinterWrapperPass<
	IRCDGPass, false, ControlDependenceGraph<Function, BasicBlock>*,
	CDGPassGraphTraits<ControlDependenceGraph<Function, BasicBlock>>> {
	static char ID;
	IRCDGPrinter() : DOTGraphTraitsPrinterWrapperPass<IRCDGPass, false,
		ControlDependenceGraph<Function, BasicBlock>*,
		CDGPassGraphTraits<ControlDependenceGraph<Function, BasicBlock>>>("ir-cdg", ID) {
		initializeIRCDGPrinterPass(*PassRegistry::getPassRegistry());
	}
};
char IRCDGPrinter::ID = 0;

struct IRCDGViewer : public DOTGraphTraitsViewerWrapperPass<
	IRCDGPass, false, ControlDependenceGraph<Function, BasicBlock>*,
	CDGPassGraphTraits<ControlDependenceGraph<Function, BasicBlock>>> {
	static char ID;
	IRCDGViewer() : DOTGraphTraitsViewerWrapperPass<IRCDGPass, false,
		ControlDependenceGraph<Function, BasicBlock>*,
		CDGPassGraphTraits<ControlDependenceGraph<Function, BasicBlock>>>("ir-cdg", ID) {
		initializeIRCDGViewerPass(*PassRegistry::getPassRegistry());
	}
};
char IRCDGViewer::ID = 0;
} //anonymous namespace

INITIALIZE_PASS_IN_GROUP(SourceCDGViewer, "view-source-cdg",
	"View Control Dependence Graph from source code", true, true,
	DefaultQueryManager::OutputPassGroup::getPassRegistry())
INITIALIZE_PASS_IN_GROUP(SourceCDGPrinter, "print-source-cdg",
	"Print Control Dependence Graph from source code", true, true,
	DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(IRCDGViewer, "view-ir-cdg",
	"View Control Dependence Graph from LLVM IR", true, true,
	DefaultQueryManager::OutputPassGroup::getPassRegistry())
INITIALIZE_PASS_IN_GROUP(IRCDGPrinter, "print-ir-cdg",
	"Print Control Dependence Graph from LLVM IR", true, true,
	DefaultQueryManager::OutputPassGroup::getPassRegistry())

FunctionPass *llvm::createSourceCDGPrinter() {
	return new SourceCDGPrinter;
}

FunctionPass *llvm::createSourceCDGViewer() {
	return new SourceCDGViewer;
}

FunctionPass *llvm::createIRCDGPrinter() {
	return new IRCDGPrinter;
}

FunctionPass *llvm::createIRCDGViewer() {
	return new IRCDGViewer;
}
