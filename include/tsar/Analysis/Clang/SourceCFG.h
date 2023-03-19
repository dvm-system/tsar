#ifndef TSAR_CLANG_INCLUDE_SOURCECFG_H
#define TSAR_CLANG_INCLUDE_SOURCECFG_H

#include "tsar/Analysis/Clang/Passes.h"
#include <bcl/utility.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <clang/AST/Decl.h>
#include <llvm/ADT/DirectedGraph.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/GraphTraits.h>
#include <llvm/Support/DOTGraphTraits.h>
#include <llvm/Pass.h>
#include <llvm/Analysis/DOTGraphTraitsPass.h>
#include <llvm/Support/GraphWriter.h>
#include <iomanip>

namespace tsar {

class SourceCFGNode;
class SourceCFGEdge;
class SourceCFG;
class SourceCFGBuilder;
using SourceCFGNodeBase=llvm::DGNode<SourceCFGNode, SourceCFGEdge>;
using SourceCFGEdgeBase=llvm::DGEdge<SourceCFGNode, SourceCFGEdge>;
using SourceCFGBase=llvm::DirectedGraph<SourceCFGNode, SourceCFGEdge>;
using StmtStringType=std::string;

struct Op {
  union {
    clang::Stmt *S;
    clang::VarDecl *VD;
  };
  bool IsStmt;
  Op(clang::VarDecl *_VD) : VD(_VD), IsStmt(false) {}
  Op(clang::Stmt *_S) : S(_S), IsStmt(true) {}
};

enum NodeOpTypes {NativeNode, WrapperNode, ReferenceNode};

struct NodeOp {
  bool isReferred;
  NodeOp() : isReferred(false) {}
  NodeOp(const NodeOp &_N) : isReferred(_N.isReferred) {}
  virtual void print(StmtStringType &) = 0;
  virtual NodeOpTypes getType() = 0;
  virtual std::string getOpAddr() = 0;
  void printRefDecl(StmtStringType &ResStr) {
    if (isReferred) {
      ResStr+="<ref_decl_"+getOpAddr();
      switch (getType()) {
        case NativeNode:
          ResStr+="_NATIVE_> - ";
          break;
        case WrapperNode:
          ResStr+="_WRAPPER_> - ";
          break;
        case ReferenceNode:
          ResStr+="_REFERENCE_> - ";
          break;
      }
    }
  }
  inline void printASTNode(StmtStringType &ResStr, tsar::Op O);
  inline void printASTVarDeclNode(StmtStringType &ResStr, clang::VarDecl *VD);
  void printASTStmtNode(StmtStringType &ResStr, clang::Stmt *S);
};

struct WrapperNodeOp : NodeOp {
  Op O;
  std::vector<NodeOp*> Leaves;
  WrapperNodeOp(tsar::Op _O) : O(_O) {}
  void print(StmtStringType &ResStr) override;
  NodeOpTypes getType() override { return WrapperNode; }
  std::string getOpAddr() override {
    return "0x"+(std::stringstream()<<std::hex<<(long unsigned)O.S).str();
  }
  ~WrapperNodeOp() {
    for (auto L : Leaves)
      if (L)
        delete L;
  }
};

struct NativeNodeOp : NodeOp {
  Op O;
  NativeNodeOp(tsar::Op _O) : O(_O) {}
  NativeNodeOp(const WrapperNodeOp &WNO) : NodeOp(WNO), O(WNO.O) {}
  void print(StmtStringType &ResStr) override {
    printRefDecl(ResStr);
    printASTNode(ResStr, O.S);
  }
  NodeOpTypes getType() override { return NativeNode; }
  std::string getOpAddr() override {
    return "0x"+(std::stringstream()<<std::hex<<(long unsigned)O.S).str();
  }
};

struct ReferenceNodeOp : NodeOp {
  NodeOp *Referred;
  std::string ReferenceName;
  ReferenceNodeOp(NodeOp *_Referred, std::string _ReferenceName)
    : Referred(_Referred), ReferenceName(_ReferenceName) {
    Referred->isReferred=true;
  }
  void print(StmtStringType &ResStr) override {
    ResStr+="<"+ReferenceName+Referred->getOpAddr()+"_REFERRENCE_>";
  }
  NodeOpTypes getType() override { return ReferenceNode; }
  std::string getOpAddr() override {
    return "0x"+(std::stringstream()<<std::hex<<(long unsigned)this).str();
  }
};

class SourceBasicBlock {
public:
  using OpStorageType=std::vector<NodeOp*>;
  using iterator=OpStorageType::iterator;
  SourceBasicBlock() = default;
  SourceBasicBlock(NodeOp *_Op) : mOps(1, _Op) {}
  SourceBasicBlock(std::initializer_list<NodeOp*> _Ops) : mOps(_Ops) {}
  SourceBasicBlock(const llvm::SmallVectorImpl<NodeOp*> &_Ops)
      : mOps(_Ops.begin(), _Ops.end()) {}
  SourceBasicBlock(const SourceBasicBlock &_SBB) = delete;
  SourceBasicBlock(SourceBasicBlock &&_SBB) = delete;
  SourceBasicBlock &operator=(const SourceBasicBlock &_SBB) = delete;
  SourceBasicBlock &operator=(SourceBasicBlock &&_SBB) = delete;
  iterator addOp(NodeOp *_Op) { mOps.push_back(_Op); return mOps.end()-1; }
  void addOpFront(NodeOp *_Op) { mOps.insert(mOps.begin(), _Op); }
  template<typename It>
  iterator addOp(It begin, It end) {
    for (auto I=begin; I!=end; ++I) mOps.push_back(*I); return mOps.end()-1;
  }
  void clear() {
    for (auto O : mOps)
      delete O;
    mOps.clear();
  }
  unsigned size() const { return mOps.size(); }
  iterator begin() { return mOps.begin(); }
  iterator end() { return mOps.end(); }
  explicit operator std::string() const;
  void decrease(int NewSize) {
    mOps.resize(NewSize);
  }
  NodeOp &operator[](int Index) { return *mOps[Index]; }
private:
  OpStorageType mOps;
};

class SourceCFGEdge : public SourceCFGEdgeBase {
public:
  enum class EdgeKind {Default, True, False, Continue, Break, ToCase};
  SourceCFGEdge(SourceCFGNode &_TargetNode, EdgeKind _Kind)
      : SourceCFGEdgeBase(_TargetNode), mKind(_Kind) {}
  SourceCFGEdge(const SourceCFGEdge &_Edge)
      : SourceCFGEdgeBase(_Edge), mKind(_Edge.mKind) {}
  SourceCFGEdge(SourceCFGEdge &&_Edge)
      : SourceCFGEdgeBase(std::move(_Edge)), mKind(_Edge.mKind) {}
  SourceCFGEdge &operator=(const SourceCFGEdge &_Edge) = default;
  EdgeKind getKind() const { return mKind; }
  explicit operator std::string() const {
    switch (mKind) {
      case EdgeKind::True:
        return "T";
      case EdgeKind::False:
        return "F";
      case EdgeKind::Default:
      default:
        return "";
    }
  }
private:
  EdgeKind mKind;
};

class SourceCFGNode : public SourceCFGNodeBase {
  friend class SourceCFG;
  friend class SourceCFGBuilder;
public:
  enum class NodeKind {Default, GraphStart, GraphStop};
  SourceCFGNode(const NodeKind _Kind) : mKind(_Kind) {}
  SourceCFGNode(const SourceCFGNode &_Node) = delete;
  SourceCFGNode(SourceCFGNode &&_Node) = delete;
  SourceCFGNode &operator=(const SourceCFGNode &_Node) = delete;
  SourceCFGNode &operator=(SourceCFGNode &&_Node) = delete;
  SourceBasicBlock::iterator addToNode(NodeOp *_Op) { return SBB.addOp(_Op); }
  template<typename It>
  SourceBasicBlock::iterator addToNode(It begin, It end) {
    return SBB.addOp(begin, end);
  }
  void addToNodeFront(NodeOp *_Op) { SBB.addOpFront(_Op); }
  NodeKind getKind() const { return mKind; }
  void setKind(NodeKind _Kind) { mKind=_Kind; }
  void merge(SourceCFGNode &NodeToAttach);
  unsigned size() { return SBB.size(); }
  explicit operator std::string() const {
    switch (mKind) {
      case NodeKind::GraphStart:
        return "START";
      case NodeKind::GraphStop:
        return "STOP";
      case NodeKind::Default:
        return (std::string)SBB;
    }
  }
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
  SourceCFG *getParent() { return OwningGraph; }
  void printAsOperand(llvm::raw_ostream &OS, bool B) {}
  NodeOp &operator[](int Index) { return SBB[Index]; }
private:
  SourceCFG *OwningGraph;
  SourceBasicBlock SBB;
  NodeKind mKind;
};

class SourceCFG : public SourceCFGBase {
public:
  SourceCFG(const std::string &_mFunctionName) : mFunctionName(_mFunctionName),
    mStartNode(&emplaceNode(SourceCFGNode::NodeKind::GraphStart)),
    mStopNode(&emplaceNode(SourceCFGNode::NodeKind::GraphStop)) {}
  ~SourceCFG() {
    for (auto N : Nodes) {
      for (auto E : N->getEdges())
        delete E;
      delete N;
    }
  }
  inline bool addNode(SourceCFGNode &N) {
    return SourceCFGBase::addNode(N)?N.OwningGraph=this, true:false;
  }
  SourceCFGNode &emplaceNode(SourceCFGNode::NodeKind _Nkind) {
    SourceCFGNode *CurrNode=new SourceCFGNode(_Nkind);
    addNode(*CurrNode);
    return *CurrNode;
  }
  inline void bindNodes(SourceCFGNode &SourceNode, SourceCFGNode &TargetNode,
      SourceCFGEdge::EdgeKind _Ekind) {
    connect(SourceNode, TargetNode, *(new SourceCFGEdge(TargetNode, _Ekind)));
  }
  inline SourceCFGNode *getStartNode() const { return mStartNode; }
  inline SourceCFGNode *getStopNode() const { return mStopNode; }
  inline llvm::StringRef getName() const { return mFunctionName; }
  void mergeNodes(SourceCFGNode &AbsorbNode, SourceCFGNode &OutgoingNode);
  void deleteEdge(SourceCFGEdge &_Edge);
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
  void deleteNode(SourceCFGNode &_Node);
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
  SourceCFGNode *splitNode(SourceCFGNode &Node, int It);
private:
  std::string mFunctionName;
  SourceCFGNode *mStartNode, *mStopNode;
};

class SourceCFGBuilder {
public:
  SourceCFGBuilder() : mSCFG(nullptr), mEntryNode(nullptr),
      mNodeToAdd(nullptr), mTreeTopParentPtr(nullptr) {}
  SourceCFG *populate(clang::FunctionDecl *Decl);
private:
  using MarkedOutsType=llvm::DenseMap<SourceCFGNode*, SourceCFGEdge::EdgeKind>;
  using OutsType=llvm::SmallPtrSet<SourceCFGNode*, 5>;
  using ParserType=void (SourceCFGBuilder::*)(clang::Stmt*);
  struct LabelInfo {
    SourceCFGNode *Node;
    int LabelIt;
  };
  struct GotoInfo {
    std::map<clang::LabelStmt*, SourceCFGNode*>::iterator GotoNodeIt;
    int GotoIt;
  };
  static bool compareLabelInfo(const LabelInfo &LI1, const LabelInfo &LI2) {
    return LI1.LabelIt<LI2.LabelIt;
  }
  static bool compareGotoInfo(const GotoInfo &GI1, const GotoInfo &GI2) {
    return GI1.GotoIt<GI2.GotoIt;
  }
  void parseCompoundStmt(clang::CompoundStmt *Stmt);
  void parseIfStmt(clang::IfStmt *Stmt);
  void parseStmt(clang::Stmt *Stmt);
  void parseDoStmt(clang::DoStmt *Root);
  void parseExpr(tsar::Op O, NodeOp *ParentOp, bool isFirstCall);
  void parseWhileStmt(clang::WhileStmt *Stmt);
  void parseBreakStmt(clang::BreakStmt *Root);
  void parseContinueStmt(clang::ContinueStmt *Root);
  void parseReturnStmt(clang::ReturnStmt *Root);
  void parseLabelStmt(clang::LabelStmt *Root);
  void parseGotoStmt(clang::GotoStmt *Root);
  void parseDeclStmt(clang::DeclStmt *Stmt, NodeOp *ParentOp);
  void parseDeclStmtWrapper(clang::DeclStmt *Stmt);
  void parseForStmt(clang::ForStmt *Root);
  void parseSwitchStmt(clang::SwitchStmt *Root);
  void parseCaseStmt(clang::CaseStmt *Root);
  void processIndirect(SourceCFGNode *CondStartNode);
  void eliminateUnreached();
  void continueFlow(tsar::NodeOp *Op);
  void processLabels();
  static std::map<clang::Stmt::StmtClass, ParserType> mParsers;
  SourceCFG *mSCFG;
  llvm::DenseMap<clang::LabelStmt*, LabelInfo> mLabels;
  llvm::DenseMap<clang::CaseStmt*, LabelInfo> mCases;
  std::map<clang::LabelStmt*, SourceCFGNode*> mGotos;
  SourceCFGNode *mEntryNode, *mNodeToAdd;
  std::stack<MarkedOutsType> mDirectOut;
  std::stack<OutsType> mContinueOut, mBreakOut;
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
    return mSCFG->getStartNode();
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
    return mSCFG->getStartNode();
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

template<> struct DOTGraphTraits<tsar::SourceCFG*> :
    public DefaultDOTGraphTraits {
  DOTGraphTraits(bool IsSimple=false) : DefaultDOTGraphTraits(IsSimple) {}
  static std::string getGraphName(const tsar::SourceCFG *mSCFG) {
    return "mSCFG";
  }
  std::string getNodeLabel(tsar::SourceCFGNode *Node, tsar::SourceCFG *mSCFG) {
    return (std::string)*Node;
  }
  std::string getEdgeSourceLabel(tsar::SourceCFGNode *Node,
      GraphTraits<tsar::SourceCFG*>::ChildIteratorType It) {
    return (std::string)**It.getCurrent();
  }
};

} //namespace llvm
#endif//TSAR_CLANG_INCLUDE_SOURCECFG_H