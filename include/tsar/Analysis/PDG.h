#ifndef TSAR_INCLUDE_BUILDPDG_H
#define TSAR_INCLUDE_BUILDPDG_H

#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Clang/SourceCFG.h"
#include <bcl/utility.h>
#include <llvm/ADT/DirectedGraph.h>
#include <llvm/Support/GenericDomTree.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/Support/DOTGraphTraits.h>
#include <llvm/Pass.h>
#include <map>

namespace tsar {

class PDGNode;
class PDGEdge;
class PDG;
class PDGBuilder;
using PDGNodeBase=llvm::DGNode<PDGNode, PDGEdge>;
using PDGEdgeBase=llvm::DGEdge<PDGNode, PDGEdge>;
using PDGBase=llvm::DirectedGraph<PDGNode, PDGEdge>;

class PDGEdge : public PDGEdgeBase {
public:
	enum class EdgeKind {ControlDependence, DataDependence};
	PDGEdge(PDGNode &_TargetNode, EdgeKind _Kind)
      : PDGEdgeBase(_TargetNode), Kind(_Kind) {}
	inline EdgeKind getKind() const { return Kind; }
private:
	EdgeKind Kind;
};

class PDGNode : public PDGNodeBase {
public:
	enum class NodeKind {Default, Region};
	PDGNode(SourceCFGNode *_mBlock)
      : mBlock(_mBlock), mKind(NodeKind::Default) {}
	inline NodeKind getKind() const { return mKind; }
	SourceCFGNode *getBlock() const { return mBlock; }
private:
	SourceCFGNode *mBlock;
	NodeKind mKind;
};

class PDG : public PDGBase {
	friend class PDGBuilder;
public:
	PDG(const std::string &_FunctionName, SourceCFG *_mSCFG)
      : FunctionName(_FunctionName), mSCFG(_mSCFG) {}

	inline bool addNode(PDGNode &N) {
		if (PDGBase::addNode(N)) {
			BlockToNodeMap.insert({N.getBlock(), &N});
			return true;
		}
		else
			return false;
	}

	PDGNode &emplaceNode(SourceCFGNode *Block) {
		PDGNode *NewNode=new PDGNode(Block);
		addNode(*NewNode);
		return *NewNode;
	}

	inline void bindNodes(PDGNode &SourceNode, PDGNode &TargetNode,
      PDGEdge::EdgeKind _Ekind) {
		connect(SourceNode, TargetNode, *(new PDGEdge(TargetNode, _Ekind)));
	}

	inline PDGNode *getNode(SourceCFGNode *Block) {
		return BlockToNodeMap[Block];
	}

	inline PDGNode *getEntryNode() {
		return getNode(mSCFG->getEntryNode());
	}

	~PDG() {
		for (auto N : Nodes) {
			for (auto E : N->getEdges())
				delete E;
			delete N;
		}
	}
private:
	std::string FunctionName;
	std::map<SourceCFGNode*, PDGNode*> BlockToNodeMap;
	SourceCFG *mSCFG;
};

class PDGBuilder {
public:
	PDGBuilder() : mPDG(nullptr) {}
	PDG *populate(SourceCFG &_SCFG);
private:
	inline void processControlDependence();
	inline llvm::DomTreeNodeBase<SourceCFGNode> *getRealRoot() {
    return *mSPDT.getRootNode()->begin();
	}
	PDG *mPDG;
	SourceCFG *mSCFG;
  llvm::PostDomTreeBase<SourceCFGNode> mSPDT;
};

}//namespace tsar

namespace llvm {

class PDGPass : public FunctionPass, private bcl::Uncopyable {
public:
	static char ID;
	PDGPass() : FunctionPass(ID), mPDG(nullptr) {
		initializePDGPassPass(*PassRegistry::getPassRegistry());
	}
	bool runOnFunction(Function &F) override;
	void getAnalysisUsage(AnalysisUsage &AU) const override;
	void releaseMemory() {
		mPDGBuilder=tsar::PDGBuilder();
		if (mPDG) {
			delete mPDG;
			mPDG=nullptr;
		}
	}
	inline tsar::PDG &getPDG() { return *mPDG; }
private:
	tsar::PDGBuilder mPDGBuilder;
	tsar::PDG *mPDG;
};

template<> struct GraphTraits<DomTreeNodeBase<tsar::SourceCFGNode>*> {
	using NodeRef=DomTreeNodeBase<tsar::SourceCFGNode>*;
	using ChildIteratorType=DomTreeNodeBase<tsar::SourceCFGNode>::iterator;
	static NodeRef getEntryNode(NodeRef N) { return N; }
	static ChildIteratorType child_begin(NodeRef N) {
		return N->begin();
	}
	static ChildIteratorType child_end(NodeRef N) {
		return N->end();
	}
};

template<bool IsPostDom> struct GraphTraits<DominatorTreeBase<
  tsar::SourceCFGNode, IsPostDom>*>
      : public GraphTraits<DomTreeNodeBase<tsar::SourceCFGNode>*> {
	static NodeRef getEntryNode(DominatorTreeBase<tsar::SourceCFGNode,
			IsPostDom> *Tree) {
		return Tree->getRootNode();
	}
	using nodes_iterator=df_iterator<DomTreeNodeBase<tsar::SourceCFGNode>*>;
	static nodes_iterator nodes_begin(DominatorTreeBase<tsar::SourceCFGNode,
			IsPostDom> *Tree) {
		return df_begin(Tree->getRootNode());
	}
	static nodes_iterator nodes_end(DominatorTreeBase<tsar::SourceCFGNode,
			IsPostDom> *Tree) {
		return df_end(Tree->getRootNode());
	}
	static unsigned size(DominatorTreeBase<tsar::SourceCFGNode,
      IsPostDom> *Tree) {
		return Tree->root_size();
	}
};

template<bool IsPostDom> struct DOTGraphTraits<DominatorTreeBase<
		tsar::SourceCFGNode, IsPostDom>*> : public DefaultDOTGraphTraits {
	DOTGraphTraits(bool IsSimple=false) : DefaultDOTGraphTraits(IsSimple) {}
	static std::string getGraphName(const DominatorTreeBase<tsar::SourceCFGNode,
			IsPostDom> *Tree) {
		return IsPostDom?"Post-Dominator Tree":"Dominator Tree";
	}
	std::string getNodeLabel(DomTreeNodeBase<tsar::SourceCFGNode> *Node,
			DominatorTreeBase<tsar::SourceCFGNode, IsPostDom> *Tree) {
		return (std::string)*Node->getBlock();
	}
	static bool isNodeHidden(DomTreeNodeBase<tsar::SourceCFGNode> *Node,
			DominatorTreeBase<tsar::SourceCFGNode, IsPostDom> *Tree) {
		Tree->isVirtualRoot(Node)?true:false;
	}
};

template<> struct GraphTraits<tsar::PDGNode*> {
	using NodeRef=tsar::PDGNode*;
	static tsar::PDGNode *PDGGetTargetNode(tsar::PDGEdge *E) {
		return &E->getTargetNode();
	}
	using ChildIteratorType=mapped_iterator<tsar::PDGNode::iterator,
			decltype(&PDGGetTargetNode)>;
	using ChildEdgeIteratorType=tsar::PDGNode::iterator;
	static NodeRef getEntryNode(NodeRef N) { return N; }
	static ChildIteratorType child_begin(NodeRef N) {
		return ChildIteratorType(N->begin(), &PDGGetTargetNode);
	}
	static ChildIteratorType child_end(NodeRef N) {
		return ChildIteratorType(N->end(), &PDGGetTargetNode);
	}
	static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
		return N->begin();
	}
	static ChildEdgeIteratorType child_edge_end(NodeRef N) { return N->end(); }
};

template<> struct GraphTraits<tsar::PDG*> :
		public GraphTraits<tsar::PDGNode*> {
	using nodes_iterator=tsar::PDG::iterator;
	static NodeRef getEntryNode(tsar::PDG *Graph) {
		return Graph->getEntryNode();
	}
	static nodes_iterator nodes_begin(tsar::PDG *Graph) {
		return Graph->begin();
	}
	static nodes_iterator nodes_end(tsar::PDG *Graph) {
		return Graph->end();
	}
	using EdgeRef=tsar::PDGEdge*;
	static NodeRef edge_dest(EdgeRef E) { return &E->getTargetNode(); }
	static unsigned size(tsar::PDG *Graph) { return Graph->size(); }
};

template<> struct DOTGraphTraits<tsar::PDG*> : public DefaultDOTGraphTraits {
	DOTGraphTraits(bool IsSimple=false) : DefaultDOTGraphTraits(IsSimple) {}
	static std::string getGraphName(const tsar::PDG *Graph) {
		return "Control Dependence Graph";
	}
	std::string getNodeLabel(const tsar::PDGNode *Node, const tsar::PDG *Graph) {
		return (std::string)*Node->getBlock();
	}
};

}//namespace llvm

#endif//TSAR_INCLUDE_BUILDPDG_H