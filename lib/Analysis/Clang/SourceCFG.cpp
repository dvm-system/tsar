#include "tsar/Analysis/Clang/SourceCFG.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Core/Query.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <llvm/Support/Casting.h>

using namespace tsar;
using namespace llvm;
using namespace clang;
using namespace std;

std::map<Stmt::StmtClass, SourceCFGBuilder::ParserType> SourceCFGBuilder::
    mParsers {
  {Stmt::StmtClass::CompoundStmtClass,(ParserType)&tsar::SourceCFGBuilder::
      parseCompoundStmt},
  {Stmt::StmtClass::IfStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseIfStmt},
  {Stmt::StmtClass::WhileStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseWhileStmt},
  {Stmt::StmtClass::DoStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseDoStmt},
  {Stmt::StmtClass::ContinueStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseContinueStmt},
  {Stmt::StmtClass::BreakStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseBreakStmt},
  {Stmt::StmtClass::ReturnStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseReturnStmt},
  {Stmt::StmtClass::LabelStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseLabelStmt},
  {Stmt::StmtClass::GotoStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseGotoStmt},
  {Stmt::StmtClass::DeclStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseDeclStmtWrapper},
  {Stmt::StmtClass::ForStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseForStmt},
  //{Stmt::StmtClass::SwitchStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      //parseSwitchStmt},    Not working
  {Stmt::StmtClass::CaseStmtClass, (ParserType)&tsar::SourceCFGBuilder::
      parseCaseStmt}
};

inline void NodeOp::printASTNode(StmtStringType &ResStr, tsar::Op O) {
  if (O.IsStmt)
    printASTStmtNode(ResStr, O.S);
  else
    printASTVarDeclNode(ResStr, O.VD);
}

inline void NodeOp::printASTVarDeclNode(StmtStringType &ResStr, VarDecl *VD) {
  ResStr+=VD->getNameAsString()+"=";
  printASTStmtNode(ResStr, VD->getInit());
}

void NodeOp::printASTStmtNode(StmtStringType &ResStr, Stmt *S) {
  switch (S->getStmtClass()) {
    case Stmt::StmtClass::UnaryOperatorClass:
      if (((clang::UnaryOperator*)S)->isPostfix()) {
        printASTNode(ResStr, ((clang::UnaryOperator*)S)->getSubExpr());
        ResStr+=((clang::UnaryOperator*)S)->getOpcodeStr(
            ((clang::UnaryOperator*)S)->getOpcode());
      }
      else {
        ResStr+=((clang::UnaryOperator*)S)->getOpcodeStr(
            ((clang::UnaryOperator*)S)->getOpcode());
        printASTNode(ResStr, ((clang::UnaryOperator*)S)->getSubExpr());
      }
      break;
    case Stmt::StmtClass::BinaryOperatorClass:
    case Stmt::StmtClass::CompoundAssignOperatorClass:
      printASTNode(ResStr, ((clang::BinaryOperator*)S)->getLHS());
      ResStr+=((clang::BinaryOperator*)S)->getOpcodeStr();
      printASTNode(ResStr, ((clang::BinaryOperator*)S)->getRHS());
      break;
    case Stmt::StmtClass::DeclRefExprClass:
      ResStr+=((DeclRefExpr*)S)->getNameInfo().getAsString();
      break;
    case Stmt::StmtClass::IntegerLiteralClass:
      {
        SmallString<5> BufferStr;
        ((IntegerLiteral*)S)->getValue().toString(BufferStr, 10, false);
        ResStr+=BufferStr;
      }
      break;
    case Stmt::StmtClass::CStyleCastExprClass:
      ResStr+=((Twine)"("+((CStyleCastExpr*)S)->getCastKindName()+")").str();
      break;
    case Stmt::StmtClass::ParenExprClass:
      ResStr+="(";
      printASTNode(ResStr, ((ParenExpr*)S)->getSubExpr());
      ResStr+=")";
      break;
    case Stmt::StmtClass::ImplicitCastExprClass:
      printASTNode(ResStr, ((ImplicitCastExpr*)S)->getSubExpr());
      break;
    case Stmt::StmtClass::IfStmtClass:
      ResStr+="if (";
      printASTNode(ResStr, ((IfStmt*)S)->getCond());
      ResStr+=")";
      break;
    case Stmt::StmtClass::DoStmtClass:
    case Stmt::StmtClass::WhileStmtClass:
      ResStr+="while (";
      printASTNode(ResStr, ((WhileStmt*)S)->getCond());
      ResStr+=")";
      break;
    case Stmt::StmtClass::NullStmtClass:
      ResStr+=";";
      break;
    case Stmt::StmtClass::ReturnStmtClass:
      if (((ReturnStmt*)S)->getRetValue()) {
        ResStr+="return ";
        printASTNode(ResStr, ((ReturnStmt*)S)->getRetValue());
      }
      else
        ResStr+="return";
      break;
    case Stmt::StmtClass::ContinueStmtClass:
      ResStr+="continue";
      break;
    case Stmt::StmtClass::BreakStmtClass:
      ResStr+="break";
      break;
    case Stmt::StmtClass::GotoStmtClass:
      ResStr+=("goto "+((GotoStmt*)S)->getLabel()->getName()).str();
      break;
    case Stmt::StmtClass::LabelStmtClass:
      ResStr+=(((LabelStmt*)S)->getDecl()->getName()+":").str();
      break;
    case Stmt::StmtClass::CaseStmtClass:
      ResStr+="case ";
      printASTNode(ResStr, ((CaseStmt*)S)->getLHS());
      ResStr+=":";
      break;
    default:
      ResStr+=S->getStmtClassName();
      break;
  }
}

void WrapperNodeOp::print(StmtStringType &ResStr) {
  printRefDecl(ResStr);
  if (O.IsStmt) {
    Expr *E=(Expr*)O.S;
    switch (E->getStmtClass()) {
      case Stmt::StmtClass::UnaryOperatorClass:
        if (((clang::UnaryOperator*)E)->isPostfix()) {
          if (Leaves[0])
            Leaves[0]->print(ResStr);
          ResStr+=((clang::UnaryOperator*)E)->getOpcodeStr(
              ((clang::UnaryOperator*)E)->getOpcode());
        }
        else {
          ResStr+=((clang::UnaryOperator*)E)->getOpcodeStr(
              ((clang::UnaryOperator*)E)->getOpcode());
          if (Leaves[0])
            Leaves[0]->print(ResStr);
        }
        break;
      case Stmt::StmtClass::CompoundAssignOperatorClass:
      case Stmt::StmtClass::BinaryOperatorClass:
        if (((clang::BinaryOperator*)E)->isCompoundAssignmentOp() || 
          !((clang::BinaryOperator*)E)->isAssignmentOp()) {
          if (Leaves[0])
            Leaves[0]->print(ResStr);
          ResStr+=((clang::BinaryOperator*)E)->getOpcodeStr();
          if (Leaves[1])
            Leaves[1]->print(ResStr);
        }
        else {
          if (Leaves[1])
            Leaves[1]->print(ResStr);
          ResStr+=((clang::BinaryOperator*)E)->getOpcodeStr();
          if (Leaves[0])
            Leaves[0]->print(ResStr);
        }
        break;
      case Stmt::StmtClass::DeclRefExprClass:
        ResStr+=((DeclRefExpr*)E)->getNameInfo().getAsString();
        break;
      case Stmt::StmtClass::IntegerLiteralClass:
        {
          SmallString<5> BufferStr;
          ((IntegerLiteral*)E)->getValue().toString(BufferStr, 10, false);
          ResStr+=BufferStr;
        }
        break;
      case Stmt::StmtClass::CStyleCastExprClass:
        ResStr+=((Twine)"("+((CStyleCastExpr*)E)->getCastKindName()+")").str();
        break;
      case Stmt::StmtClass::ParenExprClass:
        ResStr+="(";
        if (Leaves[0])
          Leaves[0]->print(ResStr);
        ResStr+=")";
        break;
      case Stmt::StmtClass::ImplicitCastExprClass:
        if (Leaves[0])
          Leaves[0]->print(ResStr);
        break;
      case Stmt::StmtClass::ConditionalOperatorClass:
        ResStr+="<cond_"+getOpAddr()+"> - ";
        if (Leaves[0])
          Leaves[0]->print(ResStr);
        break;
      case Stmt::StmtClass::IfStmtClass:
        ResStr+="if (";
        if (Leaves[0])
          Leaves[0]->print(ResStr);
        ResStr+=")";
        break;
      case Stmt::StmtClass::WhileStmtClass:
      case Stmt::StmtClass::DoStmtClass:
        ResStr+="while (";
        if (Leaves[0])
          Leaves[0]->print(ResStr);
        ResStr+=")";
        break;
      case Stmt::StmtClass::ForStmtClass:
        ResStr+="<for_stmt"+getOpAddr()+"> - ";
        if (Leaves[0])
          Leaves[0]->print(ResStr);
        break;
      case Stmt::StmtClass::SwitchStmtClass:
        ResStr+="switch (";
        if (Leaves[0])
          Leaves[0]->print(ResStr);
        ResStr+=")";
        break;
      default:
        ResStr+=O.S->getStmtClassName();
        break;
    }
  }
  else {
    ResStr+=O.VD->getNameAsString()+"=";
    if (Leaves[0])
      Leaves[0]->print(ResStr);
  }
}

SourceBasicBlock::operator string() const {
  string ResStr;
  for (auto NO : mOps) {
    NO->print(ResStr);
    ResStr+="\n";
  }
  return ResStr;
}

void SourceCFG::deleteNode(SourceCFGNode &_Node) {
  EdgeListTy EdgesToDelete;
  findIncomingEdgesToNode(_Node, EdgesToDelete);
  llvm::append_range(EdgesToDelete, _Node.getEdges());
  SourceCFGBase::removeNode(_Node);
  for (auto E : EdgesToDelete)
    delete E;
  if (mStartNode==&_Node)
    mStartNode=nullptr;
  if (mStopNode==&_Node)
    mStopNode==nullptr;
  delete &_Node;
}

void SourceCFG::deleteEdge(SourceCFGEdge &_Edge) {
  for (auto N : Nodes)
    for (auto E : N->getEdges())
      if (E==&_Edge)
        N->removeEdge(_Edge);
  delete &_Edge;
}

void SourceCFGNode::merge(SourceCFGNode &NodeToAttach) {
  for (auto E : NodeToAttach.getEdges()) {
    SourceCFGNodeBase::addEdge(*E);
    NodeToAttach.removeEdge(*E);
  }
  SBB.addOp(NodeToAttach.SBB.begin(), NodeToAttach.SBB.end());
}

SourceCFGNode *SourceCFG::splitNode(SourceCFGNode &Node, int It) {
  if (It==0)
    return &Node;
  SourceCFGNode *NewNode=&emplaceNode(SourceCFGNode::NodeKind::Default);
  for (auto E : Node.getEdges())
    NewNode->addEdge(*E);
  Node.clear();
  bindNodes(Node, *NewNode, SourceCFGEdge::EdgeKind::Default);
  NewNode->addToNode(Node.SBB.begin()+It, Node.SBB.end());
  Node.SBB.decrease(It);
  return NewNode;
}

void SourceCFG::mergeNodes(SourceCFGNode &AbsorbNode,
    SourceCFGNode &OutgoingNode) {
  EdgeListTy CommonEdges;
  if (AbsorbNode.findEdgesTo(OutgoingNode, CommonEdges)) {
    for (auto E : CommonEdges) {
      AbsorbNode.removeEdge(*E);
      delete E;
    }
    CommonEdges.clear();
  }
  if (OutgoingNode.findEdgesTo(AbsorbNode, CommonEdges))
    for (auto E : CommonEdges) {
      AbsorbNode.addEdge(*(new SourceCFGEdge(*E)));
      OutgoingNode.removeEdge(*E);
      delete E;
    }
  AbsorbNode.merge(OutgoingNode);
  SourceCFGBase::removeNode(OutgoingNode);
  delete &OutgoingNode;
}

void markReached(SourceCFGNode *Node,
    std::map<SourceCFGNode*, bool> *NodesList) {
  (*NodesList)[Node]=true;
  for (auto E : *Node)
    if (!(*NodesList)[&E->getTargetNode()])
      markReached(&E->getTargetNode(), NodesList);
}

void SourceCFGBuilder::eliminateUnreached() {
  std::map<SourceCFGNode*, bool> ReachedNodes;
  for (auto N : *mSCFG)
    ReachedNodes.insert({N, false});
  markReached(mSCFG->getStartNode(), &ReachedNodes);
  for (auto It : ReachedNodes)
    if (!It.second)
      mSCFG->deleteNode(*It.first);
}

void SourceCFGBuilder::processLabels() {
  std::map<SourceCFGNode*, vector<GotoInfo>> OrderingMap;
  for (auto GotoIt=mGotos.begin(); GotoIt!=mGotos.end(); ++GotoIt)
    if (OrderingMap.find(mLabels[GotoIt->first].Node)!=OrderingMap.end())
      OrderingMap[mLabels[GotoIt->first].Node].push_back({GotoIt,
          mLabels[GotoIt->first].LabelIt});
    else
      OrderingMap.insert({mLabels[GotoIt->first].Node,
          {{GotoIt, mLabels[GotoIt->first].LabelIt}}});
  for (auto OMIt=OrderingMap.begin(); OMIt!=OrderingMap.end(); ++OMIt) {
    auto &GIVector=OMIt->second;
    std::sort(GIVector.begin(), GIVector.end(), compareGotoInfo);
    auto GIIt=GIVector.rbegin();
    while (GIIt!=GIVector.rend()) {
      while (GIIt+1!=GIVector.rend() && (GIIt+1)->GotoIt-GIIt->GotoIt==1)
        ++GIIt;
      auto NewNode=mSCFG->splitNode(*OMIt->first, GIIt->GotoIt);
      for (auto UpdateIt=mGotos.begin(); UpdateIt!=mGotos.end(); ++UpdateIt)
        if (UpdateIt->second==OMIt->first) {
          UpdateIt->second=NewNode;
          break;
        }
      mSCFG->bindNodes(*GIIt->GotoNodeIt->second, *NewNode,
          SourceCFGEdge::EdgeKind::Default);
      ++GIIt;
    }
  }
}

SourceCFG *SourceCFGBuilder::populate(FunctionDecl *Decl) {
  if (Decl->hasBody()) {
    DeclarationNameInfo Info=Decl->getNameInfo();
    mSCFG=new SourceCFG(Info.getAsString());
    mDirectOut.push(MarkedOutsType());
    parseStmt(Decl->getBody());
    if (mEntryNode) {
      mSCFG->bindNodes(*mSCFG->getStartNode(), *mEntryNode,
          SourceCFGEdge::EdgeKind::Default);
      for (auto It : mDirectOut.top())
        mSCFG->bindNodes(*It.first, *mSCFG->getStopNode(), It.second);
      processLabels();
    }
    else
      mSCFG->bindNodes(*mSCFG->getStartNode(), *mSCFG->getStopNode(),
          SourceCFGEdge::EdgeKind::Default);
    mDirectOut.pop();
    eliminateUnreached();
  }
  return mSCFG;
}

void SourceCFGBuilder::parseStmt(Stmt *Root) {
  Stmt::StmtClass Type=Root->getStmtClass();
  if ((!mEntryNode || !mNodeToAdd) &&
    Type!=Stmt::StmtClass::CompoundStmtClass) {
    mNodeToAdd=&mSCFG->emplaceNode(SourceCFGNode::NodeKind::Default);
    if (!mEntryNode)
      mEntryNode=mNodeToAdd;
    for (auto Outs : mDirectOut.top())
      mSCFG->bindNodes(*Outs.first, *mNodeToAdd, Outs.second);
    mDirectOut.top().clear();
    mDirectOut.top().insert({mNodeToAdd, SourceCFGEdge::EdgeKind::Default});
  }
  auto ParserIt=mParsers.find(Type);
  if (ParserIt!=mParsers.end())
    (this->*(ParserIt->second))(Root);
  else {
    if (isa<clang::Expr>(Root))
      parseExpr((Expr*)Root, nullptr, true);
    else {
      mNodeToAdd->addToNode(new NativeNodeOp(Root));
      mDirectOut.top().insert({mNodeToAdd, SourceCFGEdge::EdgeKind::Default});
    }
  }
}

void SourceCFGBuilder::continueFlow(NodeOp *Op) {
  if (!mNodeToAdd) {
    mNodeToAdd=&mSCFG->emplaceNode(SourceCFGNode::NodeKind::Default);
    mNodeToAdd->addToNode(Op);
    for (auto Outs : mDirectOut.top())
      mSCFG->bindNodes(*Outs.first, *mNodeToAdd, Outs.second);
    mDirectOut.top().clear();
  }
  else
    mNodeToAdd->addToNode(Op);
  mDirectOut.top().insert({mNodeToAdd, SourceCFGEdge::EdgeKind::Default});
}

void SourceCFGBuilder::processIndirect(SourceCFGNode *CondStartNode) {
  if (CondStartNode)
    for (auto Out : mContinueOut.top())
      mSCFG->bindNodes(*Out, *CondStartNode,
          SourceCFGEdge::EdgeKind::Continue);
  for (auto Out : mBreakOut.top())
    mDirectOut.top().insert({Out, SourceCFGEdge::EdgeKind::Break});
}

void SourceCFGBuilder::parseExpr(tsar::Op O, NodeOp *ParentOp,
    bool isFirstCall) {
  SourceCFGNode *OldNodeToAdd;
  WrapperNodeOp *NewNodeOp;
  if (O.IsStmt) {
    Stmt::StmtClass Type=O.S->getStmtClass();
    if (isa<ConditionalOperator>(*O.S)) {
      MarkedOutsType UpperOuts;
      SourceCFGNode *ConditionNode, *TrueNode, *FalseNode;
      ConditionalOperator &CO=*(ConditionalOperator*)O.S;
      auto OldEntryNode=mEntryNode;
      auto OldTreeTopParentPtr=mTreeTopParentPtr;
      NodeOp *CondOp=new WrapperNodeOp(&CO);
      if (ParentOp)
        ((WrapperNodeOp*)ParentOp)->Leaves.push_back(new ReferenceNodeOp(
            CondOp, "cond_res"));
      if (mTreeTopParentPtr && ((WrapperNodeOp*)mTreeTopParentPtr)->
        Leaves.size()>0) {
        continueFlow(((WrapperNodeOp*)mTreeTopParentPtr)->Leaves[0]);
        ((WrapperNodeOp*)mTreeTopParentPtr)->Leaves[0]=new ReferenceNodeOp(
            ((WrapperNodeOp*)mTreeTopParentPtr)->Leaves[0], "");
      }
      parseExpr(CO.getCond(), CondOp, true);
      ConditionNode=mNodeToAdd;
      mEntryNode=mNodeToAdd=nullptr;
      mDirectOut.push(MarkedOutsType());
      parseStmt(CO.getTrueExpr());
      TrueNode=mEntryNode;
      mSCFG->bindNodes(*ConditionNode, *TrueNode,
          SourceCFGEdge::EdgeKind::True);
      UpperOuts=mDirectOut.top();
      mDirectOut.pop();
      for (auto It : UpperOuts)
        mDirectOut.top().insert(It);
      mEntryNode=mNodeToAdd=nullptr;
      mDirectOut.push(MarkedOutsType());
      parseStmt(CO.getFalseExpr());
      FalseNode=mEntryNode;
      mSCFG->bindNodes(*ConditionNode, *FalseNode,
          SourceCFGEdge::EdgeKind::False);
      UpperOuts=mDirectOut.top();
      mDirectOut.pop();
      for (auto It : UpperOuts)
        mDirectOut.top().insert(It);
      mDirectOut.top().erase(ConditionNode);
      mNodeToAdd=nullptr;
      mEntryNode=OldEntryNode;
      mTreeTopParentPtr=OldTreeTopParentPtr;
      if (isFirstCall && ParentOp &&
        !isa<clang::Expr>(*((WrapperNodeOp*)ParentOp)->O.S))
        continueFlow(ParentOp);
      return;
    }
    else {
      OldNodeToAdd=mNodeToAdd;
      if (isFirstCall)
        mTreeTopParentPtr=nullptr;
      NewNodeOp=new WrapperNodeOp(O.S);
      switch (Type) {
        case Stmt::StmtClass::CompoundAssignOperatorClass:
          parseExpr(((clang::BinaryOperator*)O.S)->getLHS(), NewNodeOp, false);
          parseExpr(((clang::BinaryOperator*)O.S)->getRHS(), NewNodeOp, false);
          break;
        case Stmt::StmtClass::BinaryOperatorClass:
          if (((clang::BinaryOperator*)O.S)->isAssignmentOp()) {
            parseExpr(((clang::BinaryOperator*)O.S)->getRHS(), NewNodeOp,
                false);
            parseExpr(((clang::BinaryOperator*)O.S)->getLHS(), NewNodeOp,
                false);
          }
          else {
            parseExpr(((clang::BinaryOperator*)O.S)->getLHS(), NewNodeOp,
                false);
            parseExpr(((clang::BinaryOperator*)O.S)->getRHS(), NewNodeOp,
                false);
          }
          break;
        default:
          for (auto SubExpr : O.S->children())
            parseExpr((clang::Expr*)SubExpr, NewNodeOp, false);
          break;
      }
    }
  }
  else {
    OldNodeToAdd=mNodeToAdd;
    if (isFirstCall)
      mTreeTopParentPtr=nullptr;
    NewNodeOp=new WrapperNodeOp(O.VD);
    parseExpr(O.VD->getInit(), NewNodeOp, false);
  }
  NodeOp *OpToAdd;
  if (mNodeToAdd && OldNodeToAdd==mNodeToAdd) {
    OpToAdd=new NativeNodeOp(*NewNodeOp);
    delete NewNodeOp;
  }
  else
    OpToAdd=NewNodeOp;
  if (isFirstCall) {
    if (ParentOp) {
      ((WrapperNodeOp*)ParentOp)->Leaves.push_back(OpToAdd);
      continueFlow(ParentOp);
    }
    else
      continueFlow(OpToAdd);
    mTreeTopParentPtr=nullptr;
  }
  else {
    ((WrapperNodeOp*)ParentOp)->Leaves.push_back(OpToAdd);
    mTreeTopParentPtr=ParentOp;
  }
}

void SourceCFGBuilder::parseCompoundStmt(CompoundStmt *Root) {
  MarkedOutsType UpperOuts;
  SourceCFGNode *ResultEntryNode=mEntryNode;
  mDirectOut.push(MarkedOutsType());
  for (auto S : Root->body()) {
    parseStmt(S);
    if (!ResultEntryNode && mEntryNode)
      ResultEntryNode=mEntryNode;
  }
  mEntryNode=ResultEntryNode;
  UpperOuts=mDirectOut.top();
  mDirectOut.pop();
  for (auto Outs : UpperOuts)
    mDirectOut.top().insert(Outs);
}

void SourceCFGBuilder::parseDoStmt(DoStmt *Root) {
  SourceCFGNode *mStartNode=mNodeToAdd, *CondStartNode, *CondEndNode, *Body;
  mContinueOut.push(OutsType());
  mBreakOut.push(OutsType());
  mDirectOut.push(MarkedOutsType());
  if (mStartNode->size()==0)
    Body=mStartNode;
  else {
    Body=mEntryNode=mNodeToAdd=&mSCFG->emplaceNode(
        SourceCFGNode::NodeKind::Default);
    mSCFG->bindNodes(*mStartNode, *Body, SourceCFGEdge::EdgeKind::Default);
  }
  parseStmt(Root->getBody());
  if (!mContinueOut.top().empty() || !mNodeToAdd) {
    CondStartNode=mNodeToAdd=&mSCFG->emplaceNode(
        SourceCFGNode::NodeKind::Default);
    for (auto Outs : mDirectOut.top())
      mSCFG->bindNodes(*Outs.first, *CondStartNode, Outs.second);
    mDirectOut.top().clear();
  }
  else
    CondStartNode=mNodeToAdd;
  parseExpr(Root->getCond(), new WrapperNodeOp(Root), true);
  CondEndNode=mNodeToAdd;
  mSCFG->bindNodes(*CondEndNode, *Body, SourceCFGEdge::EdgeKind::True);
  mDirectOut.pop();
  mDirectOut.top().erase(mStartNode);
  mDirectOut.top().insert({CondEndNode, SourceCFGEdge::EdgeKind::False});
  mNodeToAdd=nullptr;
  mEntryNode=mStartNode;
  processIndirect(CondStartNode);
  mContinueOut.pop();
  mBreakOut.pop();
}

void SourceCFGBuilder::parseForStmt(ForStmt *Root) {
  SourceCFGNode *mStartNode=mNodeToAdd, *CondStartNode=nullptr,
    *CondEndNode=nullptr, *Body=nullptr, *IncStartNode=nullptr, *LoopNode;
  MarkedOutsType AfterInitOuts;
  mContinueOut.push(OutsType());
  mBreakOut.push(OutsType());
  if (Root->getInit())
    if (isa<DeclStmt>(*Root->getInit()))
      parseDeclStmt((DeclStmt*)Root->getInit(), nullptr);
    else
      parseExpr(Root->getInit(), nullptr, true);
  AfterInitOuts=mDirectOut.top();
  if (Root->getCond()) {
    if (mNodeToAdd && mNodeToAdd->size()==0)
      CondStartNode=mNodeToAdd;
    else {
      CondStartNode=mNodeToAdd=&mSCFG->emplaceNode(
          SourceCFGNode::NodeKind::Default);
      for (auto Out : mDirectOut.top())
        mSCFG->bindNodes(*Out.first, *CondStartNode, Out.second);
      mDirectOut.top().clear();
    }
    parseExpr(Root->getCond(), new WrapperNodeOp(Root), true);
    CondEndNode=mNodeToAdd;
  }
  mDirectOut.top().clear();
  mEntryNode=mNodeToAdd=nullptr;
  parseStmt(Root->getBody());
  Body=mEntryNode;
  if (CondEndNode)
    mSCFG->bindNodes(*CondEndNode, *Body, SourceCFGEdge::EdgeKind::True);
  else
    for (auto Out : AfterInitOuts)
      mSCFG->bindNodes(*Out.first, *Body, Out.second);
  if (Root->getInc()) {
    if (!mContinueOut.top().empty())
      mEntryNode=mNodeToAdd=nullptr;
    parseStmt(Root->getInc());
    IncStartNode=mEntryNode;
  }
  LoopNode=CondStartNode?CondStartNode:Body;
  for (auto Out : mDirectOut.top())
    mSCFG->bindNodes(*Out.first, *LoopNode, Out.second);
  mDirectOut.top().clear();
  if (CondEndNode)
    mDirectOut.top().insert({CondEndNode, SourceCFGEdge::EdgeKind::False});
  mNodeToAdd=nullptr;
  mEntryNode=mStartNode;
  if (IncStartNode)
    processIndirect(IncStartNode);
  else
    if (CondStartNode)
      processIndirect(CondStartNode);
    else
      processIndirect(Body);
  mContinueOut.pop();
  mBreakOut.pop();
}

void SourceCFGBuilder::parseSwitchStmt(SwitchStmt *Root) {
  mBreakOut.push(OutsType());
  SourceCFGNode *CondStartNode=mNodeToAdd, *CondEndNode;
  parseExpr(Root->getCond(), new WrapperNodeOp(Root), true);
  CondEndNode=mNodeToAdd;
  mEntryNode=mNodeToAdd=nullptr;
  parseStmt(Root->getBody());
  processIndirect(nullptr);
  std::map<SourceCFGNode*, vector<int>> OrderingMap;
  for (auto CaseIt : mCases)
    if (OrderingMap.find(CaseIt.second.Node)!=OrderingMap.end())
      OrderingMap[CaseIt.second.Node].push_back(CaseIt.second.LabelIt);
    else
      OrderingMap.insert({CaseIt.second.Node, {CaseIt.second.LabelIt}});
  for (auto OMIt=OrderingMap.begin(); OMIt!=OrderingMap.end(); ++OMIt) {
    auto &IVector=OMIt->second;
    std::sort(IVector.begin(), IVector.end());
    auto It=IVector.rbegin();
    while (It!=IVector.rend()) {
      while (It+1!=IVector.rend() && *(It+1)-*It==1)
        ++It;
      auto NewNode=mSCFG->splitNode(*OMIt->first, *It);
      mSCFG->bindNodes(*CondEndNode, *NewNode,
          SourceCFGEdge::EdgeKind::ToCase);
      ++It;
    }
  }
  //mDirectOut.top().clear();
  mBreakOut.pop();
}

void SourceCFGBuilder::parseCaseStmt(CaseStmt *Root) {
  mNodeToAdd->addToNode(new NativeNodeOp(Root));
  mCases.insert({Root, {mNodeToAdd, (int)mNodeToAdd->size()-1}});
  parseStmt(Root->getSubStmt());
}

void SourceCFGBuilder::parseWhileStmt(WhileStmt *Root) {
  mContinueOut.push(OutsType());
  mBreakOut.push(OutsType());
  mDirectOut.push(MarkedOutsType());
  SourceCFGNode *mStartNode=mNodeToAdd, *CondStartNode, *CondEndNode;
  if (mStartNode->size()==0) {
    CondStartNode=mStartNode;
    parseExpr(Root->getCond(), new WrapperNodeOp(Root), true);
  }
  else {
    CondStartNode=mNodeToAdd=&mSCFG->emplaceNode(
        SourceCFGNode::NodeKind::Default);
    mSCFG->bindNodes(*mStartNode, *CondStartNode,
        SourceCFGEdge::EdgeKind::Default);
    parseExpr(Root->getCond(), new WrapperNodeOp(Root), true);
  }
  CondEndNode=mNodeToAdd;
  mDirectOut.top().erase(CondEndNode);
  mEntryNode=mNodeToAdd=nullptr;
  parseStmt(Root->getBody());
  mSCFG->bindNodes(*CondEndNode, *mEntryNode, SourceCFGEdge::EdgeKind::True);
  for (auto Outs : mDirectOut.top())
    mSCFG->bindNodes(*Outs.first, *CondStartNode, Outs.second);
  mDirectOut.pop();
  mDirectOut.top().erase(mStartNode);
  mDirectOut.top().insert({CondEndNode, SourceCFGEdge::EdgeKind::False});
  mEntryNode=mStartNode;
  mNodeToAdd=nullptr;
  processIndirect(CondStartNode);
  mContinueOut.pop();
  mBreakOut.pop();
}

void SourceCFGBuilder::parseIfStmt(IfStmt *Root) {
  MarkedOutsType UpperOuts;
  SourceCFGNode *CondStartNode=mNodeToAdd, *CondEndNode;
  Stmt *ActionStmt;
  parseExpr(Root->getCond(), new WrapperNodeOp(Root), true);
  CondEndNode=mNodeToAdd;
  mDirectOut.push(MarkedOutsType());
  mEntryNode=mNodeToAdd=nullptr;
  parseStmt(Root->getThen());
  mSCFG->bindNodes(*CondEndNode, *mEntryNode, SourceCFGEdge::EdgeKind::True);
  if ((ActionStmt=Root->getElse())) {
    mDirectOut.push(MarkedOutsType());
    mEntryNode=mNodeToAdd=nullptr;
    parseStmt(ActionStmt);
    mSCFG->bindNodes(*CondEndNode, *mEntryNode,
        SourceCFGEdge::EdgeKind::False);
    UpperOuts=mDirectOut.top();
    mDirectOut.pop();
    for (auto Out : UpperOuts)
      mDirectOut.top().insert(Out);
  }
  else
    mDirectOut.top().insert({CondEndNode, SourceCFGEdge::EdgeKind::False});
  UpperOuts=mDirectOut.top();
  mDirectOut.pop();
  mDirectOut.top().erase(CondEndNode);
  for (auto Outs : UpperOuts)
    mDirectOut.top().insert(Outs);
  mEntryNode=CondStartNode;
  mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseBreakStmt(BreakStmt *Root) {
  mNodeToAdd->addToNode(new NativeNodeOp(Root));
  mDirectOut.top().erase(mNodeToAdd);
  mBreakOut.top().insert(mNodeToAdd);
  mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseContinueStmt(ContinueStmt *Root) {
  mNodeToAdd->addToNode(new NativeNodeOp(Root));
  mDirectOut.top().erase(mNodeToAdd);
  mContinueOut.top().insert(mNodeToAdd);
  mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseGotoStmt(GotoStmt *Root) {
  mNodeToAdd->addToNode(new NativeNodeOp(Root));
  mGotos.insert({Root->getLabel()->getStmt(), mNodeToAdd});
  mDirectOut.top().erase(mNodeToAdd);
  mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseLabelStmt(LabelStmt *Root) {
  mNodeToAdd->addToNode(new NativeNodeOp(Root));
  mLabels.insert({Root, {mNodeToAdd, (int)mNodeToAdd->size()-1}});
  parseStmt(Root->getSubStmt());
}

void SourceCFGBuilder::parseReturnStmt(ReturnStmt *Root) {
  if (Root->getRetValue())
    parseExpr(Root->getRetValue(), new WrapperNodeOp(Root), true);
  else
    mNodeToAdd->addToNode(new NativeNodeOp(Root));
  mSCFG->bindNodes(*mNodeToAdd, *mSCFG->getStopNode(),
      SourceCFGEdge::EdgeKind::Default);
  mDirectOut.top().erase(mNodeToAdd);
  mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseDeclStmt(DeclStmt *Root, NodeOp *ParentOp) {
  for (auto D : Root->decls())
    if ((D->getKind()==Decl::Kind::Var) && ((VarDecl*)D)->getInit())
      parseExpr((VarDecl*)D, ParentOp, true);
}

void SourceCFGBuilder::parseDeclStmtWrapper(DeclStmt *Root) {
  parseDeclStmt(Root, nullptr);
}

char ClangSourceCFGPass::ID=0;

INITIALIZE_PASS_BEGIN(ClangSourceCFGPass, "clang-source-cfg",
  "Source Control Flow Graph (Clang)", false, true)
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_END(ClangSourceCFGPass, "clang-source-cfg",
  "Source Control Flow Graph (Clang)", false, true)

FunctionPass *createClangSourceCFGPass() { return new ClangSourceCFGPass; }

void ClangSourceCFGPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();    
}

bool ClangSourceCFGPass::runOnFunction(Function &F) {
  releaseMemory();
  auto *DISub(findMetadata(&F));
  if (!DISub)
    return false;
  auto *CU{DISub->getUnit()};
  if (!CU)
    return false;
  if (!isC(CU->getSourceLanguage()))
    return false;
  auto &TfmInfo{getAnalysis<TransformationEnginePass>()};
  auto *TfmCtx{TfmInfo ? dyn_cast_or_null<
      ClangTransformationContext>(TfmInfo->getContext(*CU)) : nullptr};
  if (!TfmCtx || !TfmCtx->hasInstance())
    return false;
  auto FuncDecl=TfmCtx->getDeclForMangledName(F.getName());
  mSCFG=mSCFGBuilder.populate((FunctionDecl*)FuncDecl);
  return false;
}

namespace {
struct ClangSourceCFGPassGraphTraits {
  static tsar::SourceCFG *getGraph(ClangSourceCFGPass *P) {
    return &P->getSourceCFG();
  }
};

struct ClangSourceCFGPrinter : public DOTGraphTraitsPrinterWrapperPass<
  ClangSourceCFGPass, false, tsar::SourceCFG*,
  ClangSourceCFGPassGraphTraits> {
  static char ID;
  ClangSourceCFGPrinter() : DOTGraphTraitsPrinterWrapperPass<ClangSourceCFGPass,
    false, tsar::SourceCFG*, ClangSourceCFGPassGraphTraits>("scfg", ID) {
    initializeClangSourceCFGPrinterPass(*PassRegistry::getPassRegistry());
  }
};
char ClangSourceCFGPrinter::ID = 0;

struct ClangSourceCFGViewer : public DOTGraphTraitsViewerWrapperPass<
  ClangSourceCFGPass, false, tsar::SourceCFG*,
  ClangSourceCFGPassGraphTraits> {
  static char ID;
  ClangSourceCFGViewer() : DOTGraphTraitsViewerWrapperPass<ClangSourceCFGPass,
    false, tsar::SourceCFG*, ClangSourceCFGPassGraphTraits>("scfg", ID) {
    initializeClangSourceCFGViewerPass(*PassRegistry::getPassRegistry());
  }
};
char ClangSourceCFGViewer::ID = 0;
} //anonymous namespace

INITIALIZE_PASS_IN_GROUP(ClangSourceCFGViewer, "clang-view-source-cfg",
  "View Source Control FLow Graph (Clang)", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

INITIALIZE_PASS_IN_GROUP(ClangSourceCFGPrinter, "clang-print-source-cfg",
  "Print Source Control FLow Graph(Clang)", true, true,
  DefaultQueryManager::OutputPassGroup::getPassRegistry())

FunctionPass *llvm::createClangSourceCFGPrinter() {
  return new ClangSourceCFGPrinter;
}

FunctionPass *llvm::createClangSourceCFGViewer() {
  return new ClangSourceCFGViewer;
}

