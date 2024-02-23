#ifndef TSAR_INCLUDE_BUILDPDG_H
#define TSAR_INCLUDE_BUILDPDG_H

#include "tsar/ADT/SpanningTreeRelation.h"
 #include "tsar/Analysis/Memory/DependenceAnalysis.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/MemoryAccessUtils.h"
#include "tsar/Analysis/Clang/SourceCFG.h"
#include "tsar/Analysis/Passes.h"
#include <llvm/ADT/DirectedGraph.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/GenericDomTree.h>
#include <llvm/Support/DOTGraphTraits.h>

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

	inline NodeType *getEntryNode() { return mEntryNode; }

	inline CFGType *getCFG() const { return mCFG; }

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

class PDGNode;
class PDGEdge;
class ProgramDependencyGraph;
using PDGNodeBase=llvm::DGNode<PDGNode, PDGEdge>;
using PDGEdgeBase=llvm::DGEdge<PDGNode, PDGEdge>;
using PDGBase=llvm::DirectedGraph<PDGNode, PDGEdge>;

class PDGNode : public PDGNodeBase {
public:
    enum class NodeKind {
        SingleInstruction,
        MultiInstruction,
        PiBlock,
        Entry,
    };
	PDGNode() : mKind(NodeKind::Entry) {}
    NodeKind getKind() const { return mKind; }
	bool collectInstructions(llvm::function_ref<bool(llvm::Instruction*)> const &Pred, llvm::SmallVectorImpl<llvm::Instruction*> &IList) const;
	bool collectEdges(llvm::function_ref<bool(const PDGEdge&)> const &Pred, llvm::SmallVectorImpl<PDGEdge*> &IList) const;
	using PDGNodeBase::findEdgeTo;
	void destroy();
	~PDGNode() = default;
protected:
	PDGNode(NodeKind Kind) : mKind(Kind) {}
    NodeKind mKind;
};

class SimplePDGNode : public PDGNode {
	friend class PDGBuilder;
public:
    SimplePDGNode(llvm::Instruction &Inst) : PDGNode(NodeKind::SingleInstruction), mInstructions({&Inst}) {}
    template<typename InputIt>
    SimplePDGNode(InputIt First, InputIt Last) : PDGNode(NodeKind::SingleInstruction), mInstructions(First, Last) {
        if (mInstructions.size()>0)
            mKind=NodeKind::MultiInstruction;
    }
    static bool classof(const PDGNode *Node) { return Node->getKind()==NodeKind::SingleInstruction ||
            Node->getKind()==NodeKind::MultiInstruction; }
	const llvm::SmallVectorImpl<llvm::Instruction*> &getInstructions() const {
		assert(!mInstructions.empty() && "Instruction List is empty.");
		return mInstructions;
	}
	llvm::SmallVectorImpl<llvm::Instruction*> &getInstructions() {
		return const_cast<llvm::SmallVectorImpl<llvm::Instruction*>&>(static_cast<const SimplePDGNode *>(this)->getInstructions());
	}
	llvm::Instruction *getFirstInstruction() const { return getInstructions().front(); }
	llvm::Instruction *getLastInstruction() const { return getInstructions().back(); }
	~SimplePDGNode() = default;
private:
	/// Append the list of instructions in \p Input to this node.
	void appendInstructions(const llvm::SmallVectorImpl<llvm::Instruction*> &Input) {
		mKind=mInstructions.size()+Input.size()>1?NodeKind::MultiInstruction:NodeKind::SingleInstruction;
		llvm::append_range(mInstructions, Input);
	}
	void appendInstructions(const SimplePDGNode &Input) {
		appendInstructions(Input.getInstructions());
	}
    /// List of instructions associated with a single or multi-instruction node.
    llvm::SmallVector<llvm::Instruction*, 3> mInstructions;
};

class PiBlockPDGNode : public PDGNode {
public:
	template<typename InputIt>
	PiBlockPDGNode(InputIt First, InputIt Last) : PDGNode(NodeKind::PiBlock), mInlinedNodes(First, Last) {}
	llvm::SmallVectorImpl<PDGNode*> &getInlinedNodes() { return mInlinedNodes; }
	const llvm::SmallVectorImpl<PDGNode*> &getInlinedNodes() const { return mInlinedNodes; }
	static bool classof(const PDGNode *Node) { return Node->getKind()==NodeKind::PiBlock; }
	~PiBlockPDGNode();
private:
	llvm::SmallVector<PDGNode*> mInlinedNodes;
};

class PDGEdge : public PDGEdgeBase {
public:
	enum class DependenceType {
		Data,
		Control,
		Last=Control
	};
    enum class EdgeKind {
		RegisterDefUse,
		Memory,
		MixedData,
        Control,
		ComplexData,
		ComplexControl
    };
    PDGEdge(PDGNode &TargetNode, DependenceType DT) : PDGEdgeBase(TargetNode),
			mKind(DT==DependenceType::Data?EdgeKind::RegisterDefUse:EdgeKind::Control) {}
    EdgeKind getKind() const { return mKind; }
	DependenceType getDependenceType() const { return (mKind==EdgeKind::Control ||
			mKind==EdgeKind::ComplexControl)?DependenceType::Control:DependenceType::Data; }
	bool isControl() const { return getDependenceType()==DependenceType::Control; }
	bool isData() const { return getDependenceType()==DependenceType::Data; }
	void destroy();
	~PDGEdge() = default;
protected:
	PDGEdge(PDGNode &TargetNode, EdgeKind Kind) : PDGEdgeBase(TargetNode), mKind(Kind) {}
private:
	EdgeKind mKind;
};

class MemoryPDGEdge : public PDGEdge {
public:
    MemoryPDGEdge(PDGNode &TargetNode, llvm::Dependence &Dep, bool HasDefUse)
            : PDGEdge(TargetNode, HasDefUse?EdgeKind::MixedData:EdgeKind::Memory), mMemoryDep(Dep) {}
    ~MemoryPDGEdge() { delete &mMemoryDep; }
    static bool classof(const PDGEdge *Edge) { return Edge->getKind()==EdgeKind::Memory ||
			Edge->getKind()==EdgeKind::MixedData; }
    const llvm::Dependence &getMemoryDep() const { return mMemoryDep; }
private:
    llvm::Dependence &mMemoryDep;
};

class ComplexPDGEdge : public PDGEdge {
public:
	enum Direction {
		Incoming,      // Incoming edges to the SCC
		Outgoing,      // Edges going ot of the SCC
		DirectionCount // To make the enum usable as an array index.
	};
	struct EdgeHandler {
		size_t SrcNOrdinal, TgtNordinal;
		PDGEdge &E;
	};
	ComplexPDGEdge(PDGNode &Dst, PDGEdge &EToInline, size_t SCCOrdinal, const Direction Dir)
			: PDGEdge(Dst, EToInline.getDependenceType()==DependenceType::Control?EdgeKind::ComplexControl:EdgeKind::ComplexData) {
		absorbEdge(EToInline, SCCOrdinal, Dir);
	}
	void absorbEdge(PDGEdge &E, size_t SCCOrdinal, const Direction Dir) {
		if (E.getKind()==PDGEdge::EdgeKind::ComplexControl ||
				E.getKind()==PDGEdge::EdgeKind::ComplexData) {
			ComplexPDGEdge &ComplexE=llvm::cast<ComplexPDGEdge>(E);
			if (Dir==Incoming)
				for (EdgeHandler &EH : ComplexE.mInlinedEdges)
					mInlinedEdges.push_back({EH.SrcNOrdinal, SCCOrdinal, EH.E});
			else
				for (EdgeHandler &EH : ComplexE.mInlinedEdges)
					mInlinedEdges.push_back({SCCOrdinal, EH.TgtNordinal, EH.E});
			ComplexE.mInlinedEdges.clear();
			E.destroy();
		}
		else
			if (Dir==Incoming)
				mInlinedEdges.push_back({0, SCCOrdinal, E});
			else
				mInlinedEdges.push_back({SCCOrdinal, 0, E});
	}
	static bool classof(const PDGEdge *Edge) { return Edge->getKind()==EdgeKind::ComplexControl ||
			Edge->getKind()==EdgeKind::ComplexData; }
	llvm::SmallVectorImpl<EdgeHandler> &getInlinedEdges() { return mInlinedEdges; }
	const llvm::SmallVectorImpl<EdgeHandler> &getInlinedEdges() const { return mInlinedEdges; }
	~ComplexPDGEdge() {
		for (EdgeHandler &EH : mInlinedEdges)
			EH.E.destroy();
	}
private:
	llvm::SmallVector<EdgeHandler> mInlinedEdges;
};

class ProgramDependencyGraph : public PDGBase {
	friend class PDGBuilder;
public:
    using NodeType=PDGNode;
    using EdgeType=PDGEdge;
    ProgramDependencyGraph(const llvm::Function &F) : PDGBase(), mF(F), mEntryNode(nullptr) {}
    llvm::StringRef getName() const { return mF.getName(); }
	PDGNode &getEntryNode() {
		assert(mEntryNode);
		return *mEntryNode;
	}
	const PDGNode &getEntryNode() const {
		assert(mEntryNode);
		return *mEntryNode;
	}
	bool addEntryNode(PDGNode &EntryNode) {
		if (addNode(EntryNode)) {
			mEntryNode=&EntryNode;
			return true;
		}
		return false;
	}
	~ProgramDependencyGraph() {
		for (PDGNode *N : Nodes) {
			for (PDGEdge *E : N->getEdges())
				E->destroy();
			N->destroy();
		}
			
	}
private:
    const llvm::Function &mF;
	PDGNode *mEntryNode;
};

llvm::raw_ostream &operator<<(llvm::raw_ostream&, const PDGNode&);
llvm::raw_ostream &operator<<(llvm::raw_ostream&, const PDGNode::NodeKind);
llvm::raw_ostream &operator<<(llvm::raw_ostream&, const PDGEdge&);
llvm::raw_ostream &operator<<(llvm::raw_ostream&, const PDGEdge::EdgeKind);
llvm::raw_ostream &operator<<(llvm::raw_ostream&, const ProgramDependencyGraph&);

class PDGBuilder {
    using MemoryLocationSet=llvm::SmallDenseSet<llvm::MemoryLocation, 2>;
public:
    PDGBuilder(ProgramDependencyGraph &G, llvm::DependenceInfo &DI, const llvm::Function &F, const AliasTree &AT,
            const DIAliasTree &DIAT, const llvm::TargetLibraryInfo &TLI, DIMemoryClientServerInfo &DIMInfo,
            bool ShouldSolveReachability=true, bool ShouldSimplify=false, bool ShouldCreatePiBlocks=false)
			: mGraph(G), mDI(DI), mF(F), mAT(AT), mDIAT(DIAT), mDIATRel(&const_cast<DIAliasTree&>(DIAT)),
            mTLI(TLI), mDIMInfo(DIMInfo), mSolvedReachability(ShouldSolveReachability), mSimplified(ShouldSimplify),
			mCreatedPiBlocks(ShouldCreatePiBlocks), mBBList(F.size()) {
		{
			size_t BBIdx=F.size();
			for (auto It=llvm::po_begin(&F); It!=llvm::po_end(&F); ++It)
				mBBList[--BBIdx]=*It;
		}
        if (mDIMInfo.isValid())
			mServerDIATRel=SpanningTreeRelation<const DIAliasTree*>(mDIMInfo.DIAT);
        if (ShouldSolveReachability)
			solveReachability();
    }
	~PDGBuilder() = default;
	void populate() {
		computeInstructionOrdinals();
		createFineGrainedNodes();
		createDefUseEdges();
		createMemoryDependenceEdges();
		createControlDependenceEdges();
		// Operation of this function results in memory leak
		// simplify();
		createPiBlocks();
	}
	static bool shouldShadow(const llvm::Instruction &I) {
		return I.isDebugOrPseudoInst() || I.isLifetimeStartOrEnd();
	}
private:
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

    bool isReachable(const llvm::BasicBlock &From, const llvm::BasicBlock &To) {
		return mReachabilityMatrix(mBBToInd[&From], mBBToInd[&To]);
	}

	void solveReachability() {
		mReachabilityMatrix=ReachabilityMatrix(mF.size());
		size_t Ind=0;
		mBBToInd.init(mF.size());
		for (auto &BB : mF) {
			// mReachabilityMatrix(Ind, Ind)=true;
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

    // Returns true if there exist memory dependence between 2 instr-ons
    bool confirmMemoryIntersect(const llvm::Instruction &SrcInst, const llvm::Instruction &DstInst);

	/// Part of code, inherited from AbstractDependenceGraphBuilder and partially redefined
	void computeInstructionOrdinals();
	void createFineGrainedNodes();
	void createDefUseEdges();
	void createMemoryDependenceEdges();
	void createControlDependenceEdges();
	void simplify();
	void createPiBlocks();
	PDGNode &createFineGrainedNode(const llvm::Instruction &I) {
        PDGNode *NewNode=new SimplePDGNode(const_cast<llvm::Instruction&>(I));
        mGraph.addNode(*NewNode);
        return *NewNode;
    }
	PDGEdge &createDefUseEdge(PDGNode &Src, PDGNode &Tgt) {
        PDGEdge *NewEdge=new PDGEdge(Tgt, PDGEdge::DependenceType::Data);
        mGraph.connect(Src, Tgt, *NewEdge);
        return *NewEdge;
    }
	bool areNodesMergeable(const PDGNode &Src, const PDGNode &Tgt) const {
		using namespace llvm;
		// Only merge two nodes if they are both simple nodes and the consecutive
		// instructions after merging belong to the same BB.
		const SimplePDGNode *SimpleSrc=dyn_cast<SimplePDGNode>(&Src);
		const SimplePDGNode *SimpleTgt=dyn_cast<SimplePDGNode>(&Tgt);
		if (!SimpleSrc || !SimpleTgt)
			return false;
		return true;
		//return SimpleSrc->getLastInstruction()->getParent()==SimpleTgt->getFirstInstruction()->getParent();
	}
	void mergeNodes(PDGNode &AbsorbN, PDGNode &OutgoingN) {
		using namespace llvm;
		PDGEdge &EdgeToFold=AbsorbN.back();
		assert(AbsorbN.getEdges().size()==1 && EdgeToFold.getTargetNode()==OutgoingN && "Expected A to have a single edge to B.");
		assert(isa<SimplePDGNode>(&AbsorbN) && isa<SimplePDGNode>(&OutgoingN) && "Expected simple nodes");
		// Copy instructions from B to the end of A.
		cast<SimplePDGNode>(&AbsorbN)->appendInstructions(*cast<SimplePDGNode>(&OutgoingN));
		// Move to A any outgoing edges from B.
		for (PDGEdge *OutgoingE : OutgoingN)
			mGraph.connect(AbsorbN, OutgoingE->getTargetNode(), *OutgoingE);
		AbsorbN.removeEdge(EdgeToFold);
		EdgeToFold.destroy();
		mGraph.removeNode(OutgoingN);
		OutgoingN.destroy();
	}
	PiBlockPDGNode &createPiBlock(const llvm::SmallVectorImpl<PDGNode*> &NodeList) {
		PiBlockPDGNode *Res=new PiBlockPDGNode(NodeList.begin(), NodeList.end());
		mGraph.addNode(*Res);
		return *Res;
	}
	size_t getOrdinal(const llvm::Instruction &I) {
		assert(mInstOrdinalMap.find(&I) != mInstOrdinalMap.end() &&
			"No ordinal computed for this instruction.");
		return mInstOrdinalMap[&I];
	}
	size_t getOrdinal(PDGNode &N) {
		assert(mNodeOrdinalMap.find(&N)!=mNodeOrdinalMap.end() &&
			"No ordinal computed for this node.");
		return mNodeOrdinalMap[&N];
	}
	llvm::DenseMap<const llvm::Instruction*, PDGNode*> mIMap;
	llvm::DenseMap<const llvm::Instruction*, size_t> mInstOrdinalMap;
	llvm::DenseMap<PDGNode*, size_t> mNodeOrdinalMap;
	llvm::DependenceInfo &mDI;
	llvm::SmallVector<const llvm::BasicBlock*, 8> mBBList;
	//

	ProgramDependencyGraph &mGraph;
    const llvm::Function &mF;
	const AliasTree &mAT;
	const DIAliasTree &mDIAT;
	SpanningTreeRelation<const DIAliasTree*> mDIATRel;
	const llvm::TargetLibraryInfo &mTLI;
	bool mSolvedReachability, mSimplified, mCreatedPiBlocks;
	ReachabilityMatrix mReachabilityMatrix;
	llvm::DenseMap<const llvm::BasicBlock*, size_t> mBBToInd;
	DIMemoryClientServerInfo &mDIMInfo;
	std::optional<SpanningTreeRelation<const DIAliasTree*>> mServerDIATRel;
};
} //namespace tsar

namespace llvm {

class ProgramDependencyGraphPass : public FunctionPass, private bcl::Uncopyable {
public:
	static char ID;
	ProgramDependencyGraphPass() : FunctionPass(ID), mPDGBuilder(nullptr), mPDG(nullptr) {
        initializeProgramDependencyGraphPassPass(*PassRegistry::getPassRegistry());
    }
	bool runOnFunction(Function &F) override;
	void getAnalysisUsage(AnalysisUsage &AU) const override;
	void releaseMemory() override {
        if (mPDGBuilder) {
            delete mPDGBuilder;
            mPDGBuilder=nullptr;
        }
		if (mPDG) {
			delete mPDG;
			mPDG=nullptr;
		}
	}
	inline tsar::ProgramDependencyGraph &getPDG() { return *mPDG; }
	inline const tsar::ProgramDependencyGraph &getPDG() const { return *mPDG; }
private:
	tsar::PDGBuilder *mPDGBuilder;
	tsar::ProgramDependencyGraph *mPDG;
};

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

template <> struct GraphTraits<tsar::PDGNode*> {
    using NodeRef=tsar::PDGNode *;
    static tsar::PDGNode *PDGGetTargetNode(DGEdge<tsar::PDGNode, tsar::PDGEdge> *P) {
        return &P->getTargetNode();
    }
    // Provide a mapped iterator so that the GraphTrait-based implementations can
    // find the target nodes without having to explicitly go through the edges.
    using ChildIteratorType =
        mapped_iterator<tsar::PDGNode::iterator, decltype(&PDGGetTargetNode)>;
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

template <>
struct GraphTraits<tsar::ProgramDependencyGraph *> : public GraphTraits<tsar::PDGNode *> {
    using nodes_iterator = tsar::ProgramDependencyGraph::iterator;
    static NodeRef getEntryNode(tsar::ProgramDependencyGraph *DG) {
        return &DG->getEntryNode();
    }
    static nodes_iterator nodes_begin(tsar::ProgramDependencyGraph *DG) {
        return DG->begin();
    }
    static nodes_iterator nodes_end(tsar::ProgramDependencyGraph *DG) { return DG->end(); }
};

template <> struct GraphTraits<const tsar::PDGNode *> {
    using NodeRef=const tsar::PDGNode *;
    static const tsar::PDGNode *PDGGetTargetNode(const DGEdge<tsar::PDGNode, tsar::PDGEdge> *P) {
        return &P->getTargetNode();
    }
    using ChildIteratorType =
        mapped_iterator<tsar::PDGNode::const_iterator, decltype(&PDGGetTargetNode)>;
    using ChildEdgeIteratorType = tsar::PDGNode::const_iterator;
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

template <>
struct GraphTraits<const tsar::ProgramDependencyGraph *>
    : public GraphTraits<const tsar::PDGNode *> {
    using nodes_iterator=tsar::ProgramDependencyGraph::const_iterator;
    static NodeRef getEntryNode(const tsar::ProgramDependencyGraph *DG) {
        return &DG->getEntryNode();
    }
    static nodes_iterator nodes_begin(const tsar::ProgramDependencyGraph *DG) {
        return DG->begin();
    }
    static nodes_iterator nodes_end(const tsar::ProgramDependencyGraph *DG) {
        return DG->end();
    }
};

template <>
struct DOTGraphTraits<const tsar::ProgramDependencyGraph *>
    : public DefaultDOTGraphTraits {
    DOTGraphTraits(bool IsSimple=false) : DefaultDOTGraphTraits(IsSimple) {}
    static std::string getGraphName(const tsar::ProgramDependencyGraph *G) {
        assert(G && "expected a valid pointer to the graph.");
        return "PDG for '"+std::string(G->getName())+"'";
    }
    std::string getNodeLabel(const tsar::PDGNode *Node, const tsar::ProgramDependencyGraph *Graph);
    std::string getEdgeAttributes(const tsar::PDGNode *Node,
			GraphTraits<const tsar::PDGNode *>::ChildIteratorType I, const tsar::ProgramDependencyGraph *G);
    static bool isNodeHidden(const tsar::PDGNode *Node, const tsar::ProgramDependencyGraph *G);
private:
    static std::string getSimpleNodeLabel(const tsar::PDGNode *Node,
			const tsar::ProgramDependencyGraph *G);
    static std::string getVerboseNodeLabel(const tsar::PDGNode *Node,
			const tsar::ProgramDependencyGraph *G);
    static std::string getVerboseEdgeAttributes(const tsar::PDGNode *Src,
			const tsar::PDGEdge *Edge, const tsar::ProgramDependencyGraph *G);
	static std::string getEdgeStyle(const tsar::PDGEdge*);
};

using PDGDotGraphTraits=DOTGraphTraits<const tsar::ProgramDependencyGraph*>;

}//namespace llvm

#endif//TSAR_INCLUDE_BUILDPDG_H