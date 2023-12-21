#ifndef TSAR_INCLUDE_BUILDPDG_H
#define TSAR_INCLUDE_BUILDPDG_H

#include "tsar/Analysis/Passes.h"
#include "tsar/Analysis/Clang/SourceCFG.h"
#include <bcl/utility.h>
#include <llvm/IR/CFG.h>
#include <llvm/ADT/DirectedGraph.h>
#include <llvm/Support/GenericDomTree.h>
#include <llvm/ADT/DepthFirstIterator.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/Support/DOTGraphTraits.h>
#include <llvm/Support/Caching.h>
#include <llvm/Pass.h>
#include <map>
#include <string>

#include <llvm/Analysis/DDG.h>
#include <llvm/Analysis/DDGPrinter.h>
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/Dominators.h>

#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"

// DEBUG:
#include <iostream>
#include <vector>
#include "tsar/ADT/SpanningTreeRelation.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Core/Query.h"
#include "tsar/Unparse/Utils.h"
#include <llvm/Analysis/DOTGraphTraitsPass.h>
#include <llvm/Support/GraphWriter.h>


namespace tsar {

template<typename CFGType, typename CFGNodeType>
class CDGNode;
template<typename CFGType, typename CFGNodeType>
class CDGEdge;
template<typename CDGType>
class CDGBuilder;

template<typename CFGType, typename CFGNodeType>
using CDGNodeBase=llvm::DGNode<CDGNode<CFGType, CFGNodeType>, CDGEdge<CFGType, CFGNodeType>>;
template<typename CFGType, typename CFGNodeType>
using CDGEdgeBase=llvm::DGEdge<CDGNode<CFGType, CFGNodeType>, CDGEdge<CFGType, CFGNodeType>>;
template<typename CFGType, typename CFGNodeType>
using CDGBase=llvm::DirectedGraph<CDGNode<CFGType, CFGNodeType>, CDGEdge<CFGType, CFGNodeType>>;

template<typename CFGType, typename CFGNodeType>
class CDGEdge : public CDGEdgeBase<CFGType, CFGNodeType> {
public:
	using NodeType=CDGNode<CFGType, CFGNodeType>;
	CDGEdge(NodeType &TargetNode) : CDGEdgeBase<CFGType, CFGNodeType>(TargetNode){}
};

template<typename CFGType, typename CFGNodeType>
class CDGNode : public CDGNodeBase<CFGType, CFGNodeType> {
public:
	enum class NodeKind {Entry, Default, Region};
	CDGNode(NodeKind Kind) : mKind(Kind) {}
	inline NodeKind getKind() const { return mKind; }
	void destroy();
private:
	NodeKind mKind;
};

template<typename CFGType, typename CFGNodeType>
class EntryCDGNode : public CDGNode<CFGType, CFGNodeType> {
private:
	using Base=CDGNode<CFGType, CFGNodeType>;
public:
	EntryCDGNode() : Base(Base::NodeKind::Entry) {}
	static bool classof(const Base *Node) { return Node->getKind()==Base::NodeKind::Entry; }
};

template<typename CFGType, typename CFGNodeType>
class DefaultCDGNode : public CDGNode<CFGType, CFGNodeType> {
private:
	using Base=CDGNode<CFGType, CFGNodeType>;
public:
	using NodeValueType=CFGNodeType*;
	DefaultCDGNode(NodeValueType Block) : Base(Base::NodeKind::Default), mBlock(Block) {}
	static bool classof(const Base *Node) { return Node->getKind()==Base::NodeKind::Default; }
	inline NodeValueType getBlock() const { return mBlock; }
private:
	NodeValueType mBlock;
};

template<typename CFGType, typename CFGNodeType>
class ControlDependenceGraph : public CDGBase<CFGType, CFGNodeType> {
private:
	using DefaultNode=DefaultCDGNode<CFGType, CFGNodeType>;
	using EntryNode=EntryCDGNode<CFGType, CFGNodeType>;
public:
	using CFGT=CFGType;
	using CFGNodeT=CFGNodeType;
	using NodeType=CDGNode<CFGType, CFGNodeType>;
	using EdgeType=CDGEdge<CFGType, CFGNodeType>;
	// TODO: NodeValueType must be declared once for all DirectedGraph classes
	using NodeValueType=CFGNodeType*;
	using CFGNodeMapType=llvm::DenseMap<NodeValueType, NodeType*>;

	ControlDependenceGraph(const std::string &FunctionName, CFGType *CFG)
			: mFunctionName(FunctionName), mCFG(CFG), mEntryNode(new EntryNode()) {
		CDGBase<CFGType, CFGNodeType>::addNode(*mEntryNode);
	}

	inline bool addNode(DefaultNode &N) {
		if (CDGBase<CFGType, CFGNodeType>::addNode(N)) {
			mBlockToNodeMap.insert({N.getBlock(), &N});
			return true;
		}
		else
			return false;
	}

	NodeType &emplaceNode(NodeValueType Block) {
		DefaultNode *NewNode=new DefaultNode(Block);
		addNode(*NewNode);
		return *NewNode;
	}

	inline void bindNodes(NodeType &SourceNode, NodeType &TargetNode) {
		CDGBase<CFGType, CFGNodeType>::connect(SourceNode, TargetNode, *(new CDGEdge(TargetNode)));
	}

	inline NodeType *getNode(NodeValueType Block) {
		return mBlockToNodeMap[Block];
	}

	inline NodeType *getEntryNode() {
		return mEntryNode;
	}

	inline CFGType *getCFG() const {
		return mCFG;
	}

	~ControlDependenceGraph() {
		for (auto N : CDGBase<CFGType, CFGNodeType>::Nodes) {
			for (auto E : N->getEdges())
				delete E;
			delete N;
		}
	}
private:
	EntryNode *mEntryNode;
	std::string mFunctionName;
	CFGNodeMapType mBlockToNodeMap;
	CFGType *mCFG;
};

using SourceCDG=ControlDependenceGraph<SourceCFG, SourceCFGNode>;
using IRCDGNode=CDGNode<llvm::Function, llvm::BasicBlock>;
using DefaultIRCDGNode=DefaultCDGNode<llvm::Function, llvm::BasicBlock>;
using EntryIRCDGNode=EntryCDGNode<llvm::Function, llvm::BasicBlock>;
using IRCDGEdge=CDGEdge<llvm::Function, llvm::BasicBlock>;
using IRCDG=ControlDependenceGraph<llvm::Function, llvm::BasicBlock>;

}//namespace tsar

namespace llvm {

template<typename CFGType, typename CFGNodeType>
struct GraphTraits<tsar::CDGNode<CFGType, CFGNodeType>*> {
	using NodeRef=tsar::CDGNode<CFGType, CFGNodeType>*;
	static tsar::CDGNode<CFGType, CFGNodeType> *CDGGetTargetNode(tsar::CDGEdge<CFGType, CFGNodeType> *E) {
		return &E->getTargetNode();
	}
	using ChildIteratorType=mapped_iterator<typename tsar::CDGNode<CFGType, CFGNodeType>::iterator,
			decltype(&CDGGetTargetNode)>;
	using ChildEdgeIteratorType=typename tsar::CDGNode<CFGType, CFGNodeType>::iterator;
	static NodeRef getEntryNode(NodeRef N) { return N; }
	static ChildIteratorType child_begin(NodeRef N) {
		return ChildIteratorType(N->begin(), &CDGGetTargetNode);
	}
	static ChildIteratorType child_end(NodeRef N) {
		return ChildIteratorType(N->end(), &CDGGetTargetNode);
	}
	static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
		return N->begin();
	}
	static ChildEdgeIteratorType child_edge_end(NodeRef N) { return N->end(); }
};

template<typename CFGType, typename CFGNodeType>
struct GraphTraits<tsar::ControlDependenceGraph<CFGType, CFGNodeType>*> :
		public GraphTraits<tsar::CDGNode<CFGType, CFGNodeType>*> {
	using nodes_iterator=typename tsar::ControlDependenceGraph<CFGType, CFGNodeType>::iterator;
	static typename llvm::GraphTraits<tsar::CDGNode<CFGType, CFGNodeType>*>::NodeRef getEntryNode(tsar::ControlDependenceGraph<CFGType, CFGNodeType> *Graph) {
		return Graph->getEntryNode();
	}
	static nodes_iterator nodes_begin(tsar::ControlDependenceGraph<CFGType, CFGNodeType> *Graph) {
		return Graph->begin();
	}
	static nodes_iterator nodes_end(tsar::ControlDependenceGraph<CFGType, CFGNodeType> *Graph) {
		return Graph->end();
	}
	using EdgeRef=tsar::CDGEdge<CFGType, CFGNodeType>*;
	static typename llvm::GraphTraits<tsar::CDGNode<CFGType, CFGNodeType>*>::NodeRef edge_dest(EdgeRef E) { return &E->getTargetNode(); }
	static unsigned size(tsar::ControlDependenceGraph<CFGType, CFGNodeType> *Graph) { return Graph->size(); }
};

template<typename CFGType, typename CFGNodeType>
struct DOTGraphTraits<tsar::ControlDependenceGraph<CFGType, CFGNodeType>*> :
		public DOTGraphTraits<CFGType*> {
private:
	using GTInstanced=GraphTraits<tsar::ControlDependenceGraph<CFGType, CFGNodeType>*>;
public:
	DOTGraphTraits(bool IsSimple=false) : DOTGraphTraits<CFGType*>(IsSimple) {}
	static std::string getGraphName(const tsar::ControlDependenceGraph<CFGType, CFGNodeType> *Graph) {
		return "Control Dependence Graph";
	}
	std::string getNodeLabel(const tsar::CDGNode<CFGType, CFGNodeType> *Node,
			const tsar::ControlDependenceGraph<CFGType, CFGNodeType> *Graph) {
		if (auto *DefNode=dyn_cast<tsar::DefaultCDGNode<CFGType, CFGNodeType>>(Node))
			return DOTGraphTraits<CFGType*>::getNodeLabel(DefNode->getBlock(), Graph->getCFG());
		else
			return "Entry";
	}
	std::string getNodeAttributes(const tsar::CDGNode<CFGType, CFGNodeType> *Node,
			tsar::ControlDependenceGraph<CFGType, CFGNodeType> *Graph) {
		if (auto *DefNode=dyn_cast<tsar::DefaultCDGNode<CFGType, CFGNodeType>>(Node))
			return DOTGraphTraits<CFGType*>::getNodeAttributes(DefNode->getBlock(), Graph->getCFG());
		else
			return "";
	}
	std::string getEdgeSourceLabel(const tsar::CDGNode<CFGType, CFGNodeType> *Node,
			typename GraphTraits<tsar::ControlDependenceGraph<CFGType, CFGNodeType>*>::ChildIteratorType It) { return ""; }
	std::string getEdgeAttributes(const tsar::CDGNode<CFGType, CFGNodeType> *Node,
			typename GTInstanced::ChildIteratorType EdgeIt,
			tsar::ControlDependenceGraph<CFGType, CFGNodeType> *Graph) {
		//return DOTGraphTraits<CFGType*>::getEdgeAttributes(Node->getBlock(), EdgeIt, Graph->getCFG());
		return "";
	}
	bool isNodeHidden(const tsar::CDGNode<CFGType, CFGNodeType> *Node,
			tsar::ControlDependenceGraph<CFGType, CFGNodeType>*) {
		return false;
	}
};

}//namespace llvm

namespace tsar {
template<typename CDGType>
class CDGBuilder {
private:
	using CFGType=typename CDGType::CFGT;
	using CFGNodeType=typename CDGType::CFGNodeT;
public:
	// TODO: NodeValueType must be declared once for all DirectedGraph classes
	using NodeValueType=typename llvm::GraphTraits<CFGType*>::NodeRef;

	CDGBuilder() : mCDG(nullptr) {}
	CDGType *populate(CFGType &_CFG);
private:
	inline void processControlDependence();
	CFGType *mCFG;
	CDGType *mCDG;
	llvm::PostDomTreeBase<CFGNodeType> mPDT;
};
}; //namespace tsar

namespace llvm {

template<typename CDGType>
class CDGPass : public FunctionPass, private bcl::Uncopyable {
private:
	using CFGType=typename CDGType::CFGT;
	using CFGNodeType=typename CDGType::CFGNodeT;
public:
	static char ID;
	CDGPass();
	bool runOnFunction(Function &F) override;
	void getAnalysisUsage(AnalysisUsage &AU) const override;
	void releaseMemory() {
		mCDGBuilder=tsar::CDGBuilder<CDGType>();
		if (mCDG) {
			delete mCDG;
			mCDG=nullptr;
		}
	}
	inline tsar::ControlDependenceGraph<CFGType, CFGNodeType> &getCDG() { return *mCDG; }
private:
	tsar::CDGBuilder<CDGType> mCDGBuilder;
	tsar::ControlDependenceGraph<CFGType, CFGNodeType> *mCDG;
};

template<>
CDGPass<tsar::ControlDependenceGraph<tsar::SourceCFG, tsar::SourceCFGNode>>::CDGPass() : FunctionPass(ID), mCDG(nullptr) {
	initializeSourceCDGPassPass(*PassRegistry::getPassRegistry());
}

template<>
CDGPass<tsar::ControlDependenceGraph<Function, BasicBlock>>::CDGPass() : FunctionPass(ID), mCDG(nullptr) {
	initializeIRCDGPassPass(*PassRegistry::getPassRegistry());
}

using SourceCDGPass=CDGPass<tsar::ControlDependenceGraph<tsar::SourceCFG, tsar::SourceCFGNode>>;
using IRCDGPass=CDGPass<tsar::ControlDependenceGraph<Function, BasicBlock>>;
};//namespace llvm

namespace tsar {
class ControlPDGEdge : public llvm::DDGEdge {
public:
	ControlPDGEdge(llvm::DDGNode &TargetNode) : llvm::DDGEdge(TargetNode, EdgeKind::Unknown) {}
};

class ProgramDependencyGraph  : public llvm::DataDependenceGraph {
public:
	ProgramDependencyGraph(llvm::IRCDGPass &P, llvm::Function &F, llvm::DependenceInfo &DI, const AliasTree &AT,
			const DIAliasTree &DIAT, const llvm::TargetLibraryInfo &TLI, bool ShouldSolveReachability=true)
			: Base(F, DI), mF(F), mAT(AT), mDIAT(DIAT), mDIATRel(&const_cast<DIAliasTree&>(DIAT)), mTLI(TLI), 
			mSolvedReachability(ShouldSolveReachability), mDIMInfo(const_cast<DIAliasTree&>(DIAT), P, F) {
		if (mDIMInfo.isValid())
			mServerDIATRel=SpanningTreeRelation<const DIAliasTree*>(mDIMInfo.DIAT);
		if (ShouldSolveReachability)
			solveReachability();
		std::vector<llvm::DDGNode*> NodesToRemove;
		std::vector<std::pair<llvm::DDGNode*, llvm::DDGEdge*>> EdgesToDelete;
		//
		int RemovedEdges=0;
		//
		for (auto INode : *this) {
			if (INode->getKind()==llvm::DDGNode::NodeKind::SingleInstruction &&
					shouldShadow(*llvm::dyn_cast<llvm::SimpleDDGNode>(INode)->getFirstInstruction())) {
				NodesToRemove.push_back(INode);
				continue;
			}
			for (auto Edge : INode->getEdges()) {
				assert(Edge->getKind()!=llvm::DDGEdge::EdgeKind::Unknown);
				const llvm::Instruction &TargetInst=*llvm::dyn_cast<llvm::SimpleDDGNode>(&Edge->getTargetNode())->getFirstInstruction();
				if (shouldShadow(TargetInst)) {
					EdgesToDelete.push_back({INode, Edge});
					continue;
				}
				// Try to exclude some memory dependence edges
				if (Edge->getKind()==llvm::DDGEdge::EdgeKind::MemoryDependence &&
						specifyMemoryDependence(*llvm::dyn_cast<llvm::SimpleDDGNode>(INode),
						Edge->getTargetNode())) {
					EdgesToDelete.push_back({INode, Edge});
					++RemovedEdges;
				}
			}
		}
		// DEBUG:
		std::cerr<<"Removed Edges Count - "<<RemovedEdges<<std::endl;
		//
		for (auto &RQ : EdgesToDelete) {
			RQ.first->removeEdge(*RQ.second);
			delete RQ.second;
		}
		for (auto INode : NodesToRemove) {
			for (auto Edge : *INode)
				delete Edge;
			Nodes.erase(findNode(*INode));
			delete INode;
		}
		IRCDG *CDG=CDGBuilder<IRCDG>().populate(mF);
		std::map<llvm::Instruction*, llvm::DDGNode*> InstrMap;
		for (auto INode : *this)
			if (llvm::isa<llvm::SimpleDDGNode>(INode))
				for (auto I : llvm::dyn_cast<llvm::SimpleDDGNode>(INode)->getInstructions())
					InstrMap[I]=INode;
		for (auto It : InstrMap)
			if (It.first->isTerminator()) {
				IRCDGNode *CDGNode=CDG->getNode(It.first->getParent());
				for (auto CDGEdge : *CDGNode) {
					llvm::BasicBlock *TargetBB=((DefaultIRCDGNode*)(&CDGEdge->getTargetNode()))->getBlock();
					for (auto IIt=TargetBB->begin(); IIt!=TargetBB->end(); ++IIt)
						if (!shouldShadow(*IIt))
							connect(*InstrMap[It.first], *InstrMap[&(*IIt)], *(new ControlPDGEdge(*InstrMap[&(*IIt)])));
				}
			}
		for (auto RootEdge : getRoot().getEdges())
			delete RootEdge;
		getRoot().clear();
		for (auto CDGEdge : *(CDG->getEntryNode()))
			for (auto IIt=((DefaultIRCDGNode*)(&CDGEdge->getTargetNode()))->getBlock()->begin();
					IIt!=((DefaultIRCDGNode*)(&CDGEdge->getTargetNode()))->getBlock()->end(); ++IIt)
				if (!shouldShadow(*IIt))
					connect(getRoot(), *InstrMap[&(*IIt)], *(new ControlPDGEdge(*InstrMap[&(*IIt)])));
		delete CDG;
	}
private:
	using Base=llvm::DataDependenceGraph;
	using MemoryLocationSet=llvm::SmallDenseSet<llvm::MemoryLocation, 2>;

	struct ReachabilityMatrix : public std::vector<bool> {
		size_t NodesCount;
		ReachabilityMatrix() = default;
		ReachabilityMatrix(size_t _NodesCount) : std::vector<bool>(_NodesCount*_NodesCount, false), NodesCount(_NodesCount) {}
		std::vector<bool>::reference operator()(size_t I, size_t J) {
			return this->operator[](I*NodesCount+J);
		}
		std::vector<bool>::const_reference operator()(size_t I, size_t J) const {
			return this->operator[](I*NodesCount+J);
		}
	};

	bool shouldShadow(const llvm::Instruction &I) {
		return I.isDebugOrPseudoInst() || I.isLifetimeStartOrEnd();
	}

	bool isReachable(const llvm::BasicBlock &From, const llvm::BasicBlock &To) {
		return mReachabilityMatrix(mBBToInd[&From], mBBToInd[&To]);
	}

	void solveReachability() {
		mReachabilityMatrix=ReachabilityMatrix(mF.size());
		size_t Ind=0;
		mBBToInd.init(mF.size());
		for (auto &BB : mF) {
			mReachabilityMatrix(Ind, Ind)=true;
			mBBToInd[&BB]=Ind++;
		}
		for (auto &BB : mF)
			for (auto SuccBB : llvm::successors(&BB))
				mReachabilityMatrix(mBBToInd[&BB], mBBToInd[SuccBB])=true;
		for (size_t K=0; K<mReachabilityMatrix.NodesCount; ++K)
			for (size_t I=0; I<mReachabilityMatrix.NodesCount; ++I)
				for (size_t J=0; J<mReachabilityMatrix.NodesCount; ++J)
					mReachabilityMatrix(I, J)=mReachabilityMatrix(I, J) | (mReachabilityMatrix(I, K) & mReachabilityMatrix(K, J));
	}

	// Returns true if caller must delete edge
	bool specifyMemoryDependence(const llvm::DDGNode &Src, const llvm::DDGNode &Dst) {
		using namespace llvm;
		using namespace std;
		const Instruction &SrcInst=*dyn_cast<SimpleDDGNode>(&Src)->getFirstInstruction(),
			&DstInst=*llvm::dyn_cast<SimpleDDGNode>(&Dst)->getFirstInstruction();
		if (!isReachable(*SrcInst.getParent(), *DstInst.getParent()))
			return true;
		bool SrcUnknownMemory=false, DstUnknownMemory=false, *CurrMarker=&SrcUnknownMemory;
		MemoryLocationSet SrcMemLocs, DstMemLocs, *CurrSet=&SrcMemLocs;
		auto CollectMemory=[&CurrSet](Instruction &I, MemoryLocation &&MemLoc, unsigned OpInd,
				AccessInfo IsRead, AccessInfo IsWrite) {
			CurrSet->insert(MemLoc);
		};
		auto EvaluateUnknown=[&CurrMarker](Instruction&, AccessInfo, AccessInfo) {
			*CurrMarker=true;
		};
		for_each_memory(const_cast<Instruction&>(SrcInst), const_cast<TargetLibraryInfo&>(mTLI),
				CollectMemory, EvaluateUnknown);
		CurrSet=&DstMemLocs;
		CurrMarker=&DstUnknownMemory;
		for_each_memory(const_cast<Instruction&>(DstInst), const_cast<TargetLibraryInfo&>(mTLI),
				CollectMemory, EvaluateUnknown);
		if (SrcMemLocs.empty() || DstMemLocs.empty())
			return SrcMemLocs.empty() && !SrcUnknownMemory || DstMemLocs.empty() && !DstUnknownMemory;
		SmallVector<DIMemory*, 2> SrcDIMems, SrcServerDIMems, DstDIMems, DstServerDIMems;
		auto FillDIMemories=[&](const MemoryLocationSet &MemoryLocations, SmallVectorImpl<DIMemory*> &DIMemories,
				SmallVectorImpl<DIMemory*> &ServerDIMemories) {
			for (auto &MemLoc : MemoryLocations) {
				const EstimateMemory *EstMem=mAT.find(MemLoc);
				const MDNode *MDNode;
				while (!(MDNode=getRawDIMemoryIfExists(*EstMem, mF.getContext(),
						mF.getParent()->getDataLayout(), mAT.getDomTree())))
					EstMem=EstMem->getParent();
				DIMemories.push_back(&*mDIAT.find(*MDNode));
				ServerDIMemories.push_back(mDIMInfo.findFromClient(*EstMem, SrcInst.getModule()->getDataLayout(),
						const_cast<DominatorTree&>(mAT.getDomTree())).get<Clone>());
			}
		};
		FillDIMemories(SrcMemLocs, SrcDIMems, SrcServerDIMems);
		FillDIMemories(DstMemLocs, DstDIMems, DstServerDIMems);
		bool ShouldDelete=true;
		for (int SrcI=0; SrcI<SrcDIMems.size() && ShouldDelete; ++SrcI) {
			const DIAliasNode *SrcDINode, *SrcServerDINode;
			for (int DstI=0; DstI<DstDIMems.size() && ShouldDelete; ++DstI) {
				const DIAliasNode *DstDINode, *DstServerDINode;
				if (SrcDIMems[SrcI] && DstDIMems[DstI] && SrcDIMems[SrcI]->hasAliasNode() && DstDIMems[DstI]->hasAliasNode() &&
						mDIATRel.compare(SrcDINode=SrcDIMems[SrcI]->getAliasNode(), DstDINode=DstDIMems[DstI]->getAliasNode())!=
						TreeRelation::TR_UNREACHABLE && (!mServerDIATRel || mServerDIATRel.value().compare(SrcServerDINode=
						SrcServerDIMems[SrcI]->getAliasNode(), DstServerDINode=DstServerDIMems[DstI]->getAliasNode())!=
						TreeRelation::TR_UNREACHABLE))
					ShouldDelete=false;
			}
		}
		if (ShouldDelete)
			return true;
		else {
			// At this point we have dependence confirmed by AliasTrees
			return false;
		}
	}

	llvm::Function &mF;
	const AliasTree &mAT;
	const DIAliasTree &mDIAT;
	SpanningTreeRelation<const DIAliasTree*> mDIATRel;
	const llvm::TargetLibraryInfo &mTLI;
	bool mSolvedReachability;
	ReachabilityMatrix mReachabilityMatrix;
	llvm::DenseMap<const llvm::BasicBlock*, size_t> mBBToInd;
	DIMemoryClientServerInfo mDIMInfo;
	std::optional<SpanningTreeRelation<const DIAliasTree*>> mServerDIATRel;
};
}

namespace llvm {
template <>
struct GraphTraits<tsar::ProgramDependencyGraph *> : public GraphTraits<DataDependenceGraph*> {};

template <>
struct GraphTraits<const tsar::ProgramDependencyGraph *> : public GraphTraits<const DataDependenceGraph*> {};

template <>
struct DOTGraphTraits<const tsar::ProgramDependencyGraph *> : public DOTGraphTraits<const DataDependenceGraph*> {
private:
	using GT=GraphTraits<const tsar::ProgramDependencyGraph *>;
	using BaseDOTGT=DOTGraphTraits<const DataDependenceGraph*>;
public:
	DOTGraphTraits(bool isSimple=false) : BaseDOTGT(isSimple) {}

	std::string getNodeLabel(const DDGNode *Node, const tsar::ProgramDependencyGraph *PDG) {
		return BaseDOTGT::getNodeLabel(Node, PDG);
	}

	std::string getEdgeAttributes(const DDGNode *Node, typename GT::ChildIteratorType ChildNodeIt, const tsar::ProgramDependencyGraph *PDG) {
		assert((*ChildNodeIt.getCurrent())->getKind()!=DDGEdge::EdgeKind::Rooted);
		switch ((*ChildNodeIt.getCurrent())->getKind()) {
			case DDGEdge::EdgeKind::Unknown:
				return "style=\"dashed\"";
			case DDGEdge::EdgeKind::RegisterDefUse:
				return "style=\"solid\" color=\"blue\" "+BaseDOTGT::getEdgeAttributes(Node, ChildNodeIt, PDG);
			case DDGEdge::EdgeKind::MemoryDependence:
				return "style=\"solid\" color=\"green\" "+BaseDOTGT::getEdgeAttributes(Node, ChildNodeIt, PDG);
		}
	}
};

};

#endif//TSAR_INCLUDE_BUILDPDG_H