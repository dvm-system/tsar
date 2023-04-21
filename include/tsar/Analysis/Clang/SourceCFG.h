#ifndef TSAR_CLANG_INCLUDE_SOURCECFG_H
#define TSAR_CLANG_INCLUDE_SOURCECFG_H

#include "tsar/Analysis/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/AST/ASTTypeTraits.h>
#include <clang/Basic/LangOptions.h>
#include <clang/AST/PrettyPrinter.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Decl.h>
#include <llvm/ADT/DirectedGraph.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/Pass.h>
#include <llvm/Analysis/DOTGraphTraitsPass.h>
#include <llvm/Support/DOTGraphTraits.h>
#include <llvm/Support/GraphWriter.h>
#include <llvm/Support/Casting.h>
#include <iomanip>
#include <map>    //For Inverse<SourceCFG>

namespace tsar {

class SourceCFGNode;
class SourceCFGEdge;
class SourceCFG;
class SourceCFGBuilder;
using SourceCFGNodeBase=llvm::DGNode<SourceCFGNode, SourceCFGEdge>;
using SourceCFGEdgeBase=llvm::DGEdge<SourceCFGNode, SourceCFGEdge>;
using SourceCFGBase=llvm::DirectedGraph<SourceCFGNode, SourceCFGEdge>;
using StmtStringType=std::string;
class ReferenceNodeOp;
class NativeNodeOp;

class NodeOp {
	friend class ReferenceNodeOp;
	friend class NativeNodeOp;
	friend class SourceCFGBuilder;
public:
	enum class NodeOpKind {Native, Wrapper, Reference};
	NodeOp(NodeOpKind _mKind) : mKind(_mKind), mIsReferred(false) {}
	NodeOp(const NodeOp &_N) = default;
	NodeOpKind getKind() const { return mKind; };
	void print(StmtStringType &ResStr) const;
	std::string getOpAddr() const;

	void printRefDecl(StmtStringType &ResStr) const {
		if (mIsReferred) {
			ResStr+="<ref_decl_"+getOpAddr();
			switch (mKind) {
				case NodeOpKind::Native:
					ResStr+="_NATIVE_> - ";
					break;
				case NodeOpKind::Wrapper:
					ResStr+="_WRAPPER_> - ";
					break;
				case NodeOpKind::Reference:
					ResStr+="_REFERENCE_> - ";
					break;
			}
		}
	}
	~NodeOp() = default;
	void destroy();
private:
	NodeOpKind mKind;
	bool mIsReferred;
};

class WrapperNodeOp : public NodeOp {
	friend class SourceCFGBuilder;
	friend class NativeNodeOp;
public:
	WrapperNodeOp(const clang::Stmt *_Op) : NodeOp(NodeOpKind::Wrapper),
      Op(clang::DynTypedNode::create(*_Op)) {}
	WrapperNodeOp(const clang::VarDecl *_Op) : NodeOp(NodeOpKind::Wrapper),
      Op(clang::DynTypedNode::create(*_Op)) {}
	void print(StmtStringType &ResStr) const;
	static bool classof(const NodeOp *Node) {
    return Node->getKind()==NodeOpKind::Wrapper;
  }

	std::string getOpAddr() const {
		return "0x"+(std::stringstream()<<std::hex<<
      (long unsigned)&Op.getUnchecked<clang::Stmt>()).str();
	}

	~WrapperNodeOp() {
		for (auto L : Leaves)
			L->destroy();
	}
private:
	clang::DynTypedNode Op;
	std::vector<NodeOp*> Leaves;
};

class NativeNodeOp : public NodeOp {
	friend class SourceCFGBuilder;
public:
	NativeNodeOp(clang::Stmt *_Op) : NodeOp(NodeOpKind::Native),
      Op(clang::DynTypedNode::create(*_Op)) {}
	NativeNodeOp(clang::VarDecl *_Op) : NodeOp(NodeOpKind::Native),
      Op(clang::DynTypedNode::create(*_Op)) {}
	NativeNodeOp(const WrapperNodeOp &WNO) : NodeOp(WNO), Op(WNO.Op) {
    mKind=NodeOpKind::Native;
  }
	static bool classof(const NodeOp *Node) {
		return Node->getKind()==NodeOpKind::Native;
	}

	void print(StmtStringType &ResStr) const {
		llvm::raw_string_ostream StrStream(ResStr);
		printRefDecl(ResStr);
		if (Op.get<clang::Stmt>()) {
			Op.getUnchecked<clang::Stmt>().printPretty(StrStream, nullptr,
          clang::PrintingPolicy(clang::LangOptions()));
		}
		else
			Op.getUnchecked<clang::VarDecl>().print(StrStream);
	}

	std::string getOpAddr() const {
		return "0x"+(std::stringstream()<<std::hex<<
      (long unsigned)&Op.getUnchecked<clang::Stmt>()).str();
	}
	~NativeNodeOp() = default;
private:
	clang::DynTypedNode Op;
};

class ReferenceNodeOp : public NodeOp {
	friend class SourceCFGBuilder;
public:
	ReferenceNodeOp(NodeOp *_mReferredNode, std::string _mReferenceName)
		: NodeOp(NodeOpKind::Reference), mReferredNode(_mReferredNode),
    mReferenceName(_mReferenceName) {
		mReferredNode->mIsReferred=true;
	}
	static bool classof(const NodeOp *Node) {
    return Node->getKind()==NodeOpKind::Reference;
  }
	void print(StmtStringType &ResStr) const {
		ResStr+="<"+mReferenceName+mReferredNode->getOpAddr()+"_REFERRENCE_>";
	}
	std::string getOpAddr() const {
		return "0x"+(std::stringstream()<<std::hex<<(long unsigned)this).str();
	}
	~ReferenceNodeOp() = default;
private:
	NodeOp *mReferredNode;
	std::string mReferenceName;
};

class SourceCFGEdge : public SourceCFGEdgeBase {
public:
	enum class EdgeKind {Default, Labeled};
	SourceCFGEdge(SourceCFGNode &_TargetNode, EdgeKind _Kind)
			: SourceCFGEdgeBase(_TargetNode), mKind(_Kind) {}

	EdgeKind getKind() const { return mKind; }

	explicit operator std::string() const;
	void destroy();
	~SourceCFGEdge() = default;
private:
	EdgeKind mKind;
};

class DefaultSCFGEdge : public SourceCFGEdge {
public:
	enum class EdgeType {Default, True, False, ImplicitDefault};
	DefaultSCFGEdge(SourceCFGNode &_TargetNode, EdgeType _mType)
      : SourceCFGEdge(_TargetNode, EdgeKind::Default), mType(_mType) {}

	explicit operator std::string() const {
		switch (mType) {
			case EdgeType::Default:
				return "";
			case EdgeType::True:
				return "T";
			case EdgeType::False:
				return "F";
			case EdgeType::ImplicitDefault:
				return "default";
		}
	}

	~DefaultSCFGEdge() = default;
private:
	EdgeType mType;
};

class LabeledSCFGEdge : public SourceCFGEdge {
public:
	LabeledSCFGEdge(SourceCFGNode &_TargetNode, clang::SwitchCase *_mLabel)
			: SourceCFGEdge(_TargetNode, EdgeKind::Labeled), mLabel(_mLabel) {}
	
	explicit operator std::string() const {
		std::string ResStr;
		llvm::raw_string_ostream StrStream(ResStr);
		if (llvm::isa<clang::DefaultStmt>(*mLabel))
			ResStr="default";
		else
			((clang::CaseStmt*)mLabel)->getLHS()->printPretty(StrStream, nullptr,
          clang::PrintingPolicy(clang::LangOptions()));
		return ResStr;
	}

	~LabeledSCFGEdge() = default;
private:
	clang::SwitchCase *mLabel;
};

class SourceCFGNode : public SourceCFGNodeBase {
	friend class SourceCFG;
public:
	enum class NodeKind {Default, Service};
	SourceCFGNode(NodeKind _mKind) : mKind(_mKind) {}
	NodeKind getKind() const { return mKind; }

	inline SourceCFGEdge *addNewEdge(SourceCFGNode &TargetNode,
			SourceCFGEdge::EdgeKind Ekind) {
		SourceCFGEdge *ResEdge=new SourceCFGEdge(TargetNode, Ekind);
		if (addEdge(*ResEdge))
			return ResEdge;
		else {
			delete ResEdge;
			return nullptr;
		}
	}

	void merge(SourceCFGNode &NodeToAttach);
	SourceCFG *getParent() { return OwningGraph; }
	explicit operator std::string() const;
	void printAsOperand(llvm::raw_ostream &OS, bool B) {
    OS<<operator std::string();
  }
	~SourceCFGNode() = default;
	void destroy();
private:
	NodeKind mKind;
	SourceCFG *OwningGraph;
};

class ServiceSCFGNode : public SourceCFGNode {
public:
	enum class NodeType {GraphStart, GraphStop, GraphEntry};
	ServiceSCFGNode(NodeType _mType) : SourceCFGNode(NodeKind::Service),
      mType(_mType) {}
	NodeType getType() const { return mType; }
	static bool classof(const SourceCFGNode *Node) {
    return Node->getKind()==NodeKind::Service;
  }
	explicit operator std::string() const {
		switch (mType) {
			case NodeType::GraphStart:
				return "START";
			case NodeType::GraphStop:
				return "STOP";
			case NodeType::GraphEntry:
				return "ENTRY";
		}
	}
private:
	NodeType mType;
};

class DefaultSCFGNode : public SourceCFGNode {
	friend class SourceCFG;
	friend class SourceCFGBuilder;
public:
	using OpStorageType=std::vector<NodeOp*>;
	DefaultSCFGNode() : SourceCFGNode(NodeKind::Default) {}
	std::size_t size() const { return mBlock.size(); }
	static bool classof(const SourceCFGNode *Node) {
    return Node->getKind()==NodeKind::Default;
  }
	NodeOp &operator[](int Index) { return *mBlock[Index]; }
	explicit operator std::string() const;

	void clearBlock() {
		for (auto Op : mBlock)
			Op->destroy();
		mBlock.clear();
	}

	~DefaultSCFGNode() {
		clearBlock();
	}

private:
	inline OpStorageType::iterator addOp(NodeOp *_Op) {
    mBlock.push_back(_Op);
    return mBlock.end()-1;
  }

	template<typename It>
	OpStorageType::iterator addOp(It begin, It end) {
		for (auto I=begin; I!=end; ++I)
      mBlock.push_back(*I);
    return mBlock.end()-1;
	}

	OpStorageType mBlock;
};

class SourceCFG : public SourceCFGBase {
public:
	SourceCFG(const std::string &_mFunctionName) : mFunctionName(_mFunctionName),
		mStartNode(&emplaceNode(ServiceSCFGNode::NodeType::GraphStart)),
		mStopNode(&emplaceNode(ServiceSCFGNode::NodeType::GraphStop)),
		mEntryNode(nullptr) {}


	inline bool addNode(SourceCFGNode &N) {
		return SourceCFGBase::addNode(N)?N.OwningGraph=this, true:false;
	}

	DefaultSCFGNode &emplaceNode() {
		DefaultSCFGNode *CurrNode=new DefaultSCFGNode();
		addNode(*CurrNode);
		return *CurrNode;
	}

	ServiceSCFGNode &emplaceNode(ServiceSCFGNode::NodeType Type) {
		ServiceSCFGNode *CurrNode=new ServiceSCFGNode(Type);
		addNode(*CurrNode);
		return *CurrNode;
	}

	ServiceSCFGNode &emplaceEntryNode() {
		mEntryNode=new ServiceSCFGNode(ServiceSCFGNode::NodeType::GraphEntry);
		addNode(*mEntryNode);
		bindNodes(*mEntryNode, *mStartNode, DefaultSCFGEdge::EdgeType::True);
		bindNodes(*mEntryNode, *mStopNode, DefaultSCFGEdge::EdgeType::False);
		return *mEntryNode;
	}

	inline void bindNodes(SourceCFGNode &SourceNode,
      SourceCFGNode &TargetNode ) {
		connect(SourceNode, TargetNode, *(new DefaultSCFGEdge(TargetNode,
        DefaultSCFGEdge::EdgeType::Default)));
	}
	
	inline void bindNodes(SourceCFGNode &SourceNode, SourceCFGNode &TargetNode,
			DefaultSCFGEdge::EdgeType EdgeType) {
		connect(SourceNode, TargetNode, *(new DefaultSCFGEdge(TargetNode,
        EdgeType)));
	}

	inline void bindNodes(SourceCFGNode &SourceNode, SourceCFGNode &TargetNode,
			clang::SwitchCase *Label) {
		connect(SourceNode, TargetNode, *(new LabeledSCFGEdge(TargetNode, Label)));
	}

	inline ServiceSCFGNode *getStartNode() const { return mStartNode; }
	inline ServiceSCFGNode *getStopNode() const { return mStopNode; }
	inline ServiceSCFGNode *getEntryNode() const {
    return mEntryNode?mEntryNode:mStartNode;
  }
	inline llvm::StringRef getName() const { return mFunctionName; }
	void mergeNodes(SourceCFGNode &AbsorbNode, SourceCFGNode &OutgoingNode);
	void deleteNode(SourceCFGNode &_Node);
	void deleteEdge(SourceCFGEdge &_Edge);
	DefaultSCFGNode *splitNode(DefaultSCFGNode &Node, int It);
	void recalculatePredMap();

	std::set<SourceCFGNode*> &getPredMap(SourceCFGNode *Node) {
		return mPredecessorsMap[Node];
	}

	bool findParentNodes(const SourceCFGNode &RequestedNode,
			llvm::SmallVectorImpl<SourceCFGNode*> &ParentNodes) {
		bool Result=false;
		for (auto N : *this)
			if (N->hasEdgeTo(RequestedNode)) {
				ParentNodes.push_back(N);
				Result=true;
			}
		return Result;
	}

	void view(const SourceCFGNode &SCFGN,
			const llvm::Twine &Name="source cfg") const {
		llvm::ViewGraph(this, Name, false,
				llvm::DOTGraphTraits<const SourceCFG*>::getGraphName(this));
	}

	std::string write(const SourceCFGNode &SCFGN,
			const llvm::Twine &Name="source cfg") const {
		return llvm::WriteGraph(this, Name, false,
				llvm::DOTGraphTraits<const SourceCFG*>::getGraphName(this));
	}

	~SourceCFG() {
		for (auto N : Nodes) {
			for (auto E : N->getEdges())
				E->destroy();
			N->destroy();
		}
	}
private:
	std::string mFunctionName;
	ServiceSCFGNode *mStartNode, *mStopNode, *mEntryNode;
	std::map<SourceCFGNode*, std::set<SourceCFGNode*>> mPredecessorsMap;
};

class SourceCFGBuilder {
public:
	SourceCFGBuilder() : mSCFG(nullptr), mEntryNode(nullptr),
			mNodeToAdd(nullptr), mTreeTopParentPtr(nullptr),
      mPrevFirstLabel({nullptr, 0}) {}
	SourceCFG *populate(clang::FunctionDecl *Decl);
private:
	using MarkedOutsType=llvm::DenseMap<SourceCFGNode*,
    DefaultSCFGEdge::EdgeType>;
	using OutsType=llvm::SmallPtrSet<SourceCFGNode*, 5>;

	struct LabelInfo {
		DefaultSCFGNode *Node;
		size_t LabelIt;
	};

	friend bool operator<(const LabelInfo&, const LabelInfo&);
	void parseCompoundStmt(clang::CompoundStmt *Root);
	void parseIfStmt(clang::IfStmt *Root);
	void parseStmt(clang::Stmt *Root);
	void parseDoStmt(clang::DoStmt *Root);
	void parseExpr(clang::DynTypedNode Op, NodeOp *ParentOp, bool isFirstCall);
	void parseWhileStmt(clang::WhileStmt *Root);
	void parseBreakStmt(clang::BreakStmt *Root);
	void parseContinueStmt(clang::ContinueStmt *Root);
	void parseReturnStmt(clang::ReturnStmt *Root);
	void parseLabelStmt(clang::Stmt *Root);
	void parseGotoStmt(clang::GotoStmt *Root);
	void parseDeclStmt(clang::DeclStmt *Root, NodeOp *ParentOp);
	void parseForStmt(clang::ForStmt *Root);
	void parseSwitchStmt(clang::SwitchStmt *Root);
	void processIndirect(SourceCFGNode *CondStartNode);
	void eliminateUnreached();
	void continueFlow(tsar::NodeOp *Op);
	void processLabels();
	bool hasConditionalOperator(clang::Stmt *Root);
	SourceCFG *mSCFG;
	llvm::SmallDenseMap<clang::Stmt*, LabelInfo> mLabels;
	llvm::SmallVector<std::pair<clang::Stmt*, DefaultSCFGNode*>> mGotos;
	llvm::SmallVector<std::pair<clang::SwitchCase*,
    DefaultSCFGNode*>> mSwitchGotos;
	LabelInfo mPrevFirstLabel;
	size_t mLastLabelIt;
	DefaultSCFGNode *mEntryNode, *mNodeToAdd;
	std::vector<MarkedOutsType> mDirectOut;
	std::stack<OutsType> mContinueOut, mBreakOut;
	std::stack<DefaultSCFGNode*> mSwitchNodes;
	NodeOp *mTreeTopParentPtr;
};
} //namespace tsar

namespace llvm {

class ClangSourceCFGPass : public FunctionPass, private bcl::Uncopyable {
public:
	static char ID;
	ClangSourceCFGPass() : FunctionPass(ID), mSCFG(nullptr) {
		initializeClangSourceCFGPassPass(*PassRegistry::getPassRegistry());
	}
	bool runOnFunction(Function &F) override;
	void getAnalysisUsage(AnalysisUsage &AU) const override;
	void releaseMemory() {
		mSCFGBuilder=tsar::SourceCFGBuilder();
		if (mSCFG) {
			delete mSCFG;
			mSCFG=nullptr;
		}
	}
	inline tsar::SourceCFG &getSourceCFG() { return *mSCFG; }
private:
	tsar::SourceCFGBuilder mSCFGBuilder;
	tsar::SourceCFG *mSCFG;
};

template<> struct GraphTraits<tsar::SourceCFGNode*> {
	using NodeRef=tsar::SourceCFGNode*;
	static tsar::SourceCFGNode *SCFGGetTargetNode(tsar::SourceCFGEdge *E) {
		return &E->getTargetNode();
	}
	using ChildIteratorType=mapped_iterator<tsar::SourceCFGNode::iterator,
			decltype(&SCFGGetTargetNode)>;
	using ChildEdgeIteratorType=tsar::SourceCFGNode::iterator;
	static NodeRef getEntryNode(NodeRef N) { return N; }
	static ChildIteratorType child_begin(NodeRef N) {
		return ChildIteratorType(N->begin(), &SCFGGetTargetNode);
	}
	static ChildIteratorType child_end(NodeRef N) {
		return ChildIteratorType(N->end(), &SCFGGetTargetNode);
	}
	static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
		return N->begin();
	}
	static ChildEdgeIteratorType child_edge_end(NodeRef N) { return N->end(); }
};

template<> struct GraphTraits<tsar::SourceCFG*> :
		public GraphTraits<tsar::SourceCFGNode*> {
	using nodes_iterator=tsar::SourceCFG::iterator;
	static NodeRef getEntryNode(tsar::SourceCFG *mSCFG) {
		return mSCFG->getEntryNode();
	}
	static nodes_iterator nodes_begin(tsar::SourceCFG *mSCFG) {
		return mSCFG->begin();
	}
	static nodes_iterator nodes_end(tsar::SourceCFG *mSCFG) {
		return mSCFG->end();
	}
	using EdgeRef=tsar::SourceCFGEdge*;
	static NodeRef edge_dest(EdgeRef E) { return &E->getTargetNode(); }
	static unsigned size(tsar::SourceCFG *mSCFG) { return mSCFG->size(); }
};

template<> struct GraphTraits<const tsar::SourceCFGNode*> {
	using NodeRef=const tsar::SourceCFGNode*;
	static const tsar::SourceCFGNode *SCFGGetTargetNode(
			const tsar::SourceCFGEdge *E) { return &E->getTargetNode(); }
	using ChildIteratorType=mapped_iterator<tsar::SourceCFGNode::const_iterator,
			decltype(&SCFGGetTargetNode)>;
	using ChildEdgeIteratorType=tsar::SourceCFGNode::const_iterator;
	static NodeRef getEntryNode(NodeRef N) { return N; }
	static ChildIteratorType child_begin(NodeRef N) {
		return ChildIteratorType(N->begin(), &SCFGGetTargetNode);
	}
	static ChildIteratorType child_end(NodeRef N) {
		return ChildIteratorType(N->end(), &SCFGGetTargetNode);
	}
	static ChildEdgeIteratorType child_edge_begin(NodeRef N) {
		return N->begin();
	}
	static ChildEdgeIteratorType child_edge_end(NodeRef N) { return N->end(); }
};

template<> struct GraphTraits<const tsar::SourceCFG*> :
		public GraphTraits<const tsar::SourceCFGNode*> {
	using nodes_iterator=tsar::SourceCFG::const_iterator;
	static NodeRef getEntryNode(const tsar::SourceCFG *mSCFG) {
		return mSCFG->getEntryNode();
	}
	static nodes_iterator nodes_begin(const tsar::SourceCFG *mSCFG) {
		return mSCFG->begin();
	}
	static nodes_iterator nodes_end(const tsar::SourceCFG *mSCFG) {
		return mSCFG->end();
	}
	using EdgeRef=const tsar::SourceCFGEdge*;
	static NodeRef edge_dest(EdgeRef E) { return &E->getTargetNode(); }
	static unsigned size(const tsar::SourceCFG *mSCFG) { return mSCFG->size(); }
};

template<> struct GraphTraits<Inverse<tsar::SourceCFGNode*>> {
	using NodeRef=tsar::SourceCFGNode*;
	using ChildIteratorType=std::set<tsar::SourceCFGNode*>::iterator;
	static ChildIteratorType child_begin(NodeRef N) {
		return N->getParent()->getPredMap(N).begin();
	}
	static ChildIteratorType child_end(NodeRef N) {
		return N->getParent()->getPredMap(N).end();
	}
	static NodeRef getEntryNode(NodeRef N) { return N; }	
};

template<> struct GraphTraits<Inverse<tsar::SourceCFG*>> :
		public GraphTraits<Inverse<tsar::SourceCFGNode*>> {
	using nodes_iterator=tsar::SourceCFG::iterator;
	static NodeRef getEntryNode(tsar::SourceCFG *SCFG) {
		return SCFG->getEntryNode();
	}
	static nodes_iterator nodes_begin(Inverse<tsar::SourceCFG*> *ISCFG) {
		return ISCFG->Graph->begin();
	}
	static nodes_iterator nodes_end(Inverse<tsar::SourceCFG*> *ISCFG) {
		return ISCFG->Graph->end();
	}
	unsigned size(Inverse<tsar::SourceCFG*> *ISCFG) {
		return ISCFG->Graph->size();
	}
};

template<> struct DOTGraphTraits<tsar::SourceCFG*> :
		public DefaultDOTGraphTraits {
	DOTGraphTraits(bool IsSimple=false) : DefaultDOTGraphTraits(IsSimple) {}
	static std::string getGraphName(const tsar::SourceCFG *SCFG) {
		return "Source Control Flow Graph";
	}
	std::string getNodeLabel(tsar::SourceCFGNode *Node, tsar::SourceCFG *SCFG) {
		return (std::string)*Node;
	}
	std::string getEdgeSourceLabel(tsar::SourceCFGNode *Node,
			GraphTraits<tsar::SourceCFG*>::ChildIteratorType It) {
		return (std::string)**It.getCurrent();
	}
};

} //namespace llvm
#endif//TSAR_CLANG_INCLUDE_SOURCECFG_H