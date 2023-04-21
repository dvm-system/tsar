#include "tsar/Analysis/Clang/SourceCFG.h"
#include "tsar/Support/PassGroupRegistry.h"
#include "tsar/Core/Query.h"
#include "tsar/Analysis/Memory/Utils.h"
#include "tsar/Support/MetadataUtils.h"
#include "tsar/Core/TransformationContext.h"
#include "tsar/Frontend/Clang/TransformationContext.h"
#include <clang/AST/Expr.h>

namespace tsar {
bool operator<(const SourceCFGBuilder::LabelInfo &LI1,
    const SourceCFGBuilder::LabelInfo &LI2) {
	return LI1.Node<LI2.Node || (LI1.Node==LI2.Node && LI1.LabelIt>LI2.LabelIt);
}
};

using namespace tsar;
using namespace llvm;
using namespace clang;
using namespace std;

template<typename KeyT, typename ValueT, typename MapT>
void addToMap(MapT &Map, KeyT Key, ValueT Value) {
    auto It=Map.find(Key);
    if (It!=Map.end())
        Map[Key].push_back(Value);
    else
        Map.insert({Key, {Value}});
}

void NodeOp::print(StmtStringType &ResStr) const {
	switch (mKind) {
		case NodeOpKind::Native:
			((NativeNodeOp*)this)->print(ResStr);
			break;
		case NodeOpKind::Wrapper:
			((WrapperNodeOp*)this)->print(ResStr);
			break;
		case NodeOpKind::Reference:
			((ReferenceNodeOp*)this)->print(ResStr);
			break;
	}
}

std::string NodeOp::getOpAddr() const {
	switch (mKind) {
		case NodeOpKind::Native:
			return ((NativeNodeOp*)this)->getOpAddr();
		case NodeOpKind::Wrapper:
			return ((WrapperNodeOp*)this)->getOpAddr();
		case NodeOpKind::Reference:
			return ((ReferenceNodeOp*)this)->getOpAddr();
	}
}

void NodeOp::destroy() {
	switch (mKind) {
		case NodeOpKind::Native:
			delete (NativeNodeOp*)this;
			break;
		case NodeOpKind::Wrapper:
			delete (WrapperNodeOp*)this;
			break;
		case NodeOpKind::Reference:
			delete (ReferenceNodeOp*)this;
			break;
	}
}

void WrapperNodeOp::print(StmtStringType &ResStr) const {
	printRefDecl(ResStr);
	if (Op.get<Stmt>()) {
		Expr *E=(Expr*)&Op.getUnchecked<Stmt>();
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
			case Stmt::StmtClass::LabelStmtClass:
				ResStr+=((Twine)Op.getUnchecked<LabelStmt>().getName()+":").str();
				break;
			case Stmt::StmtClass::CaseStmtClass:
				{
					ResStr+="case ";
					llvm::raw_string_ostream StrStream(ResStr);
					Op.getUnchecked<CaseStmt>().getLHS()->printPretty(StrStream, nullptr,
              clang::PrintingPolicy(clang::LangOptions()));
					ResStr+=":";
				}
				break;
			case Stmt::StmtClass::DefaultStmtClass:
				ResStr+="default:";
				break;
			default:
				ResStr+=Op.get<Stmt>()->getStmtClassName();
				break;
		}
	}
	else {
		ResStr+=Op.get<VarDecl>()->getNameAsString()+"=";
		if (Leaves[0])
			Leaves[0]->print(ResStr);
	}
}

void SourceCFGNode::destroy() {
	switch (mKind) {
		case NodeKind::Default:
			delete (DefaultSCFGNode*)this;
			break;
		case NodeKind::Service:
			delete (ServiceSCFGNode*)this;
			break;
	}
}

SourceCFGNode::operator std::string() const {
	switch (mKind) {
		case NodeKind::Default:
			return (string)(*(DefaultSCFGNode*)this);
		case NodeKind::Service:
			return (string)(*(ServiceSCFGNode*)this);
	}
}

DefaultSCFGNode::operator string() const {
	string ResStr;
	for (NodeOp *NO : mBlock) {
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
		E->destroy();
	if (mStartNode==&_Node)
		mStartNode=nullptr;
	if (mStopNode==&_Node)
		mStopNode==nullptr;
	_Node.destroy();
}

void SourceCFG::deleteEdge(SourceCFGEdge &_Edge) {
	for (auto N : Nodes)
		for (auto E : N->getEdges())
			if (E==&_Edge)
				N->removeEdge(_Edge);
	_Edge.destroy();
}

/* - Not working
void SourceCFGNode::merge(SourceCFGNode &NodeToAttach) {
	for (auto E : NodeToAttach.getEdges()) {
		SourceCFGNodeBase::addEdge(*E);
		NodeToAttach.removeEdge(*E);
	}
	SBB.addOp(NodeToAttach.SBB.begin(), NodeToAttach.SBB.end());
}
*/

DefaultSCFGNode *SourceCFG::splitNode(DefaultSCFGNode &Node, int It) {
	if (It==0)
		return &Node;
	DefaultSCFGNode *NewNode=&emplaceNode();
	for (auto E : Node.getEdges())
		NewNode->addEdge(*E);
	Node.clear();
	bindNodes(Node, *NewNode);
	NewNode->addOp(Node.mBlock.begin()+It, Node.mBlock.end());
	Node.mBlock.resize(It);
	return NewNode;
}

void SourceCFGEdge::destroy() {
	switch (mKind) {
		case EdgeKind::Default:
			delete (DefaultSCFGEdge*)this;
			break;
		case EdgeKind::Labeled:
			delete (LabeledSCFGEdge*)this;
			break;
	}
}

SourceCFGEdge::operator std::string() const {
	switch (mKind) {
		case EdgeKind::Default:
			return (string)(*(DefaultSCFGEdge*)this);
		case EdgeKind::Labeled:
			return (string)(*(LabeledSCFGEdge*)this);
	}
}

void SourceCFG::mergeNodes(SourceCFGNode &AbsorbNode,
		SourceCFGNode &OutgoingNode) {
	EdgeListTy CommonEdges;
	if (AbsorbNode.findEdgesTo(OutgoingNode, CommonEdges)) {
		for (auto E : CommonEdges) {
			AbsorbNode.removeEdge(*E);
			E->destroy();
		}
		CommonEdges.clear();
	}
	if (OutgoingNode.findEdgesTo(AbsorbNode, CommonEdges))
		for (auto E : CommonEdges) {
			AbsorbNode.addEdge(*(new SourceCFGEdge(*E)));
			OutgoingNode.removeEdge(*E);
			E->destroy();
		}
	//AbsorbNode.merge(OutgoingNode); - Not working
	SourceCFGBase::removeNode(OutgoingNode);
	OutgoingNode.destroy();
}

void markReached(SourceCFGNode *Node,
		std::map<SourceCFGNode*, bool> *NodesList) {
	(*NodesList)[Node]=true;
	for (auto E : *Node)
		if (!(*NodesList)[&E->getTargetNode()])
			markReached(&E->getTargetNode(), NodesList);
}

void SourceCFG::recalculatePredMap() {
	mPredecessorsMap.clear();
	for (auto N : Nodes) {
		mPredecessorsMap.insert({N, {}});
		auto OutcomingEdges=N->getEdges();
		for (auto E : OutcomingEdges) {
			auto It=mPredecessorsMap.find(&E->getTargetNode());
			if (It!=mPredecessorsMap.end())
				It->second.insert(N);
			else
				mPredecessorsMap.insert({&E->getTargetNode(), {N}});
		}
	}
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
	std::map<LabelInfo, std::vector<pair<DefaultSCFGNode*,
      SwitchCase*>>> OrderingMap;
	for (auto GotoIt : mGotos)
		addToMap(OrderingMap, mLabels[GotoIt.first], pair{GotoIt.second, nullptr});
	for (auto SwitchIt : mSwitchGotos)
		addToMap(OrderingMap, mLabels[SwitchIt.first], pair{SwitchIt.second,
        SwitchIt.first});
	for (auto It : OrderingMap) {
		auto NewNode=mSCFG->splitNode(*It.first.Node, It.first.LabelIt);
		for (auto GotoNode : It.second)
			if (GotoNode.second)
				mSCFG->bindNodes(*GotoNode.first, *NewNode, GotoNode.second);
			else
				mSCFG->bindNodes(*GotoNode.first, *NewNode);
	}
}

SourceCFG *SourceCFGBuilder::populate(FunctionDecl *Decl) {
	if (Decl->hasBody()) {
		DeclarationNameInfo Info=Decl->getNameInfo();
		mSCFG=new SourceCFG(Info.getAsString());
		mDirectOut.push_back(MarkedOutsType());
		parseStmt(Decl->getBody());
		if (mEntryNode) {
			mSCFG->bindNodes(*mSCFG->getStartNode(), *mEntryNode);
			for (auto It : mDirectOut.back())
				mSCFG->bindNodes(*It.first, *mSCFG->getStopNode(), It.second);
			processLabels();
		}
		else
			mSCFG->bindNodes(*mSCFG->getStartNode(), *mSCFG->getStopNode());
		mDirectOut.pop_back();
		eliminateUnreached();
	}
	return mSCFG;
}

bool SourceCFGBuilder::hasConditionalOperator(clang::Stmt *Root) {
	if (isa<ConditionalOperator>(*Root))
		return true;
	else {
		for (auto SubExpr : Root->children())
			if (hasConditionalOperator(SubExpr))
				return true;
		return false;
	}
}

void SourceCFGBuilder::parseStmt(Stmt *Root) {
	Stmt::StmtClass Type=Root->getStmtClass();
	if ((!mEntryNode || !mNodeToAdd) &&
		Type!=Stmt::StmtClass::CompoundStmtClass) {
		mNodeToAdd=&mSCFG->emplaceNode();
		if (!mEntryNode)
			mEntryNode=mNodeToAdd;
		for (auto Outs : mDirectOut.back())
			mSCFG->bindNodes(*Outs.first, *mNodeToAdd, Outs.second);
		mDirectOut.back().clear();
		mDirectOut.back().insert({mNodeToAdd, DefaultSCFGEdge::EdgeType::Default});
	}
	switch (Type) {
		case Stmt::StmtClass::CompoundStmtClass:
			parseCompoundStmt((CompoundStmt*)Root);
			break;
		case Stmt::StmtClass::IfStmtClass:
			parseIfStmt((IfStmt*)Root);
			break;
		case Stmt::StmtClass::WhileStmtClass:
			parseWhileStmt((WhileStmt*)Root);
			break;
		case Stmt::StmtClass::DoStmtClass:
			parseDoStmt((DoStmt*)Root);
			break;
		case Stmt::StmtClass::ContinueStmtClass:
			parseContinueStmt((ContinueStmt*)Root);
			break;
		case Stmt::StmtClass::BreakStmtClass:
			parseBreakStmt((BreakStmt*)Root);
			break;
		case Stmt::StmtClass::ReturnStmtClass:
			parseReturnStmt((ReturnStmt*)Root);
			break;
		case Stmt::StmtClass::LabelStmtClass:
			parseLabelStmt((LabelStmt*)Root);
			break;
		case Stmt::StmtClass::GotoStmtClass:
			parseGotoStmt((GotoStmt*)Root);
			break;
		case Stmt::StmtClass::DeclStmtClass:
			parseDeclStmt((DeclStmt*)Root, nullptr);
			break;
		case Stmt::StmtClass::ForStmtClass:
			parseForStmt((ForStmt*)Root);
			break;
		case Stmt::StmtClass::SwitchStmtClass:
			parseSwitchStmt((SwitchStmt*)Root);
			break;
		case Stmt::StmtClass::DefaultStmtClass:
			mDirectOut[mDirectOut.size()-2].erase(mSwitchNodes.top());
		case Stmt::StmtClass::CaseStmtClass:
			parseLabelStmt((LabelStmt*)Root);
			mSwitchGotos.push_back({(SwitchCase*)Root, mSwitchNodes.top()});
			break;
		default:
			if (isa<Expr>(*Root) && hasConditionalOperator(Root))
				parseExpr(DynTypedNode::create(*Root), nullptr, true);
			else {
				mNodeToAdd->addOp(new NativeNodeOp(Root));
				mDirectOut.back().insert({mNodeToAdd,
            DefaultSCFGEdge::EdgeType::Default});
			}
	}
}

void SourceCFGBuilder::continueFlow(NodeOp *Op) {
	if (!mNodeToAdd) {
		mNodeToAdd=&mSCFG->emplaceNode();
		mNodeToAdd->addOp(Op);
		for (auto Outs : mDirectOut.back())
			mSCFG->bindNodes(*Outs.first, *mNodeToAdd, Outs.second);
		mDirectOut.back().clear();
	}
	else
		mNodeToAdd->addOp(Op);
	mDirectOut.back().insert({mNodeToAdd, DefaultSCFGEdge::EdgeType::Default});
}

void SourceCFGBuilder::processIndirect(SourceCFGNode *CondStartNode) {
	// Processing of continue statements
	if (CondStartNode)
		for (auto Out : mContinueOut.top())
			mSCFG->bindNodes(*Out, *CondStartNode);
	// Processing of break statements
	for (auto Out : mBreakOut.top())
		mDirectOut.back().insert({Out, DefaultSCFGEdge::EdgeType::Default});
}

void SourceCFGBuilder::parseExpr(clang::DynTypedNode Op, NodeOp *ParentOp,
		bool isFirstCall) {
	DefaultSCFGNode *OldNodeToAdd;
	WrapperNodeOp *NewNodeOp;
	if (Op.get<Stmt>()) {
		Stmt::StmtClass Type=Op.getUnchecked<Stmt>().getStmtClass();
		if (Op.get<ConditionalOperator>()) {
			MarkedOutsType UpperOuts;
			DefaultSCFGNode *ConditionNode, *TrueNode, *FalseNode;
			const ConditionalOperator &CO=Op.getUnchecked<ConditionalOperator>();
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
			parseExpr(DynTypedNode::create(*CO.getCond()), CondOp, true);
			ConditionNode=mNodeToAdd;
			mEntryNode=mNodeToAdd=nullptr;
			mDirectOut.push_back(MarkedOutsType());
			parseStmt(CO.getTrueExpr());
			TrueNode=mEntryNode;
			mSCFG->bindNodes(*ConditionNode, *TrueNode,
					DefaultSCFGEdge::EdgeType::True);
			UpperOuts=mDirectOut.back();
			mDirectOut.pop_back();
			for (auto It : UpperOuts)
				mDirectOut.back().insert(It);
			mEntryNode=mNodeToAdd=nullptr;
			mDirectOut.push_back(MarkedOutsType());
			parseStmt(CO.getFalseExpr());
			FalseNode=mEntryNode;
			mSCFG->bindNodes(*ConditionNode, *FalseNode,
					DefaultSCFGEdge::EdgeType::False);
			UpperOuts=mDirectOut.back();
			mDirectOut.pop_back();
			for (auto It : UpperOuts)
				mDirectOut.back().insert(It);
			mDirectOut.back().erase(ConditionNode);
			mNodeToAdd=nullptr;
			mEntryNode=OldEntryNode;
			mTreeTopParentPtr=OldTreeTopParentPtr;
			if (isFirstCall && ParentOp &&
				!((WrapperNodeOp*)ParentOp)->Op.get<Expr>())
				continueFlow(ParentOp);
			return;
		}
		else {
			OldNodeToAdd=mNodeToAdd;
			if (isFirstCall)
				mTreeTopParentPtr=nullptr;
			NewNodeOp=new WrapperNodeOp(&Op.getUnchecked<Stmt>());
			switch (Type) {
				case Stmt::StmtClass::CompoundAssignOperatorClass:
					parseExpr(DynTypedNode::create(*Op.getUnchecked<clang::
							BinaryOperator>().getLHS()), NewNodeOp, false);
					parseExpr(DynTypedNode::create(*Op.getUnchecked<clang::
							BinaryOperator>().getRHS()), NewNodeOp, false);
					break;
				case Stmt::StmtClass::BinaryOperatorClass:
					if (Op.getUnchecked<clang::BinaryOperator>().isAssignmentOp()) {
						parseExpr(DynTypedNode::create(*Op.getUnchecked<clang::
								BinaryOperator>().getRHS()), NewNodeOp, false);
						parseExpr(DynTypedNode::create(*Op.getUnchecked<clang::
								BinaryOperator>().getLHS()), NewNodeOp, false);
					}
					else {
						parseExpr(DynTypedNode::create(*Op.getUnchecked<clang::
								BinaryOperator>().getLHS()), NewNodeOp, false);
						parseExpr(DynTypedNode::create(*Op.getUnchecked<clang::
								BinaryOperator>().getRHS()), NewNodeOp, false);
					}
					break;
				default:
					for (auto SubExpr : Op.getUnchecked<Stmt>().children())
						parseExpr(DynTypedNode::create(*SubExpr), NewNodeOp, false);
					break;
			}
		}
	}
	else {
		OldNodeToAdd=mNodeToAdd;
		if (isFirstCall)
			mTreeTopParentPtr=nullptr;
		NewNodeOp=new WrapperNodeOp(&Op.getUnchecked<VarDecl>());
		parseExpr(DynTypedNode::create(*Op.getUnchecked<VarDecl>().getInit()),
        NewNodeOp, false);
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
	DefaultSCFGNode *ResultEntryNode=mEntryNode;
	mDirectOut.push_back(MarkedOutsType());
	for (auto S : Root->body()) {
		parseStmt(S);
		if (!ResultEntryNode && mEntryNode)
			ResultEntryNode=mEntryNode;
	}
	mEntryNode=ResultEntryNode;
	UpperOuts=mDirectOut.back();
	mDirectOut.pop_back();
	for (auto Outs : UpperOuts)
		mDirectOut.back().insert(Outs);
}

void SourceCFGBuilder::parseDoStmt(DoStmt *Root) {
	DefaultSCFGNode *mStartNode=mNodeToAdd, *CondStartNode, *CondEndNode, *Body;
	mContinueOut.push(OutsType());
	mBreakOut.push(OutsType());
	mDirectOut.push_back(MarkedOutsType());
	if (mStartNode->size()==0)
		Body=mStartNode;
	else {
		Body=mEntryNode=mNodeToAdd=&mSCFG->emplaceNode();
		mSCFG->bindNodes(*mStartNode, *Body);
	}
	parseStmt(Root->getBody());
	if (!mContinueOut.top().empty() || !mNodeToAdd) {
		CondStartNode=mNodeToAdd=&mSCFG->emplaceNode();
		for (auto Outs : mDirectOut.back())
			mSCFG->bindNodes(*Outs.first, *CondStartNode, Outs.second);
		mDirectOut.back().clear();
	}
	else
		CondStartNode=mNodeToAdd;
	parseExpr(DynTypedNode::create(*Root->getCond()), new WrapperNodeOp(Root),
      true);
	CondEndNode=mNodeToAdd;
	mSCFG->bindNodes(*CondEndNode, *Body, DefaultSCFGEdge::EdgeType::True);
	mDirectOut.pop_back();
	mDirectOut.back().erase(mStartNode);
	mDirectOut.back().insert({CondEndNode, DefaultSCFGEdge::EdgeType::False});
	mNodeToAdd=nullptr;
	mEntryNode=mStartNode;
	processIndirect(CondStartNode);
	mContinueOut.pop();
	mBreakOut.pop();
}

void SourceCFGBuilder::parseForStmt(ForStmt *Root) {
	DefaultSCFGNode *mStartNode=mNodeToAdd, *CondStartNode=nullptr,
		*CondEndNode=nullptr, *Body=nullptr, *IncStartNode=nullptr, *LoopNode;
	MarkedOutsType AfterInitOuts;
	mContinueOut.push(OutsType());
	mBreakOut.push(OutsType());
	if (Root->getInit())
		if (isa<DeclStmt>(*Root->getInit()))
			parseDeclStmt((DeclStmt*)Root->getInit(), nullptr);
		else
			parseExpr(DynTypedNode::create(*Root->getInit()), nullptr, true);
	AfterInitOuts=mDirectOut.back();
	if (Root->getCond()) {
		if (mNodeToAdd && mNodeToAdd->size()==0)
			CondStartNode=mNodeToAdd;
		else {
			CondStartNode=mNodeToAdd=&mSCFG->emplaceNode();
			for (auto Out : mDirectOut.back())
				mSCFG->bindNodes(*Out.first, *CondStartNode, Out.second);
			mDirectOut.back().clear();
		}
		parseExpr(DynTypedNode::create(*Root->getCond()), new WrapperNodeOp(Root),
        true);
		CondEndNode=mNodeToAdd;
	}
	mDirectOut.back().clear();
	mEntryNode=mNodeToAdd=nullptr;
	parseStmt(Root->getBody());
	Body=mEntryNode;
	if (CondEndNode)
		mSCFG->bindNodes(*CondEndNode, *Body, DefaultSCFGEdge::EdgeType::True);
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
	for (auto Out : mDirectOut.back())
		mSCFG->bindNodes(*Out.first, *LoopNode, Out.second);
	mDirectOut.back().clear();
	if (CondEndNode)
		mDirectOut.back().insert({CondEndNode, DefaultSCFGEdge::EdgeType::False});
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
	DefaultSCFGNode *CondStartNode=mNodeToAdd, *CondEndNode;
	parseExpr(DynTypedNode::create(*Root->getCond()), new WrapperNodeOp(Root),
      true);
	CondEndNode=mNodeToAdd;
	mSwitchNodes.push(CondEndNode);
	mDirectOut.back()[CondEndNode]=DefaultSCFGEdge::EdgeType::ImplicitDefault;
	mEntryNode=mNodeToAdd=nullptr;
	parseStmt(Root->getBody());
	processIndirect(nullptr);
	mSwitchNodes.pop();
	mBreakOut.pop();
}

void SourceCFGBuilder::parseWhileStmt(WhileStmt *Root) {
	mContinueOut.push(OutsType());
	mBreakOut.push(OutsType());
	mDirectOut.push_back(MarkedOutsType());
	DefaultSCFGNode *mStartNode=mNodeToAdd, *CondStartNode, *CondEndNode;
	if (mStartNode->size()==0) {
		CondStartNode=mStartNode;
		parseExpr(DynTypedNode::create(*Root->getCond()), new WrapperNodeOp(Root),
        true);
	}
	else {
		CondStartNode=mNodeToAdd=&mSCFG->emplaceNode();
		mSCFG->bindNodes(*mStartNode, *CondStartNode);
		parseExpr(DynTypedNode::create(*Root->getCond()), new WrapperNodeOp(Root),
        true);
	}
	CondEndNode=mNodeToAdd;
	mDirectOut.back().erase(CondEndNode);
	mEntryNode=mNodeToAdd=nullptr;
	parseStmt(Root->getBody());
	mSCFG->bindNodes(*CondEndNode, *mEntryNode, DefaultSCFGEdge::EdgeType::True);
	for (auto Outs : mDirectOut.back())
		mSCFG->bindNodes(*Outs.first, *CondStartNode, Outs.second);
	mDirectOut.pop_back();
	mDirectOut.back().erase(mStartNode);
	mDirectOut.back().insert({CondEndNode, DefaultSCFGEdge::EdgeType::False});
	mEntryNode=mStartNode;
	mNodeToAdd=nullptr;
	processIndirect(CondStartNode);
	mContinueOut.pop();
	mBreakOut.pop();
}

void SourceCFGBuilder::parseIfStmt(IfStmt *Root) {
	MarkedOutsType UpperOuts;
	DefaultSCFGNode *CondStartNode=mNodeToAdd, *CondEndNode;
	Stmt *ActionStmt;
	parseExpr(DynTypedNode::create(*Root->getCond()), new WrapperNodeOp(Root),
      true);
	CondEndNode=mNodeToAdd;
	mDirectOut.push_back(MarkedOutsType());
	mEntryNode=mNodeToAdd=nullptr;
	parseStmt(Root->getThen());
	mSCFG->bindNodes(*CondEndNode, *mEntryNode, DefaultSCFGEdge::EdgeType::True);
	if ((ActionStmt=Root->getElse())) {
		mDirectOut.push_back(MarkedOutsType());
		mEntryNode=mNodeToAdd=nullptr;
		parseStmt(ActionStmt);
		mSCFG->bindNodes(*CondEndNode, *mEntryNode,
				DefaultSCFGEdge::EdgeType::False);
		UpperOuts=mDirectOut.back();
		mDirectOut.pop_back();
		for (auto Out : UpperOuts)
			mDirectOut.back().insert(Out);
	}
	else
		mDirectOut.back().insert({CondEndNode, DefaultSCFGEdge::EdgeType::False});
	UpperOuts=mDirectOut.back();
	mDirectOut.pop_back();
	mDirectOut.back().erase(CondEndNode);
	for (auto Outs : UpperOuts)
		mDirectOut.back().insert(Outs);
	mEntryNode=CondStartNode;
	mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseBreakStmt(BreakStmt *Root) {
	mNodeToAdd->addOp(new NativeNodeOp(Root));
	mDirectOut.back().erase(mNodeToAdd);
	mBreakOut.top().insert(mNodeToAdd);
	mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseContinueStmt(ContinueStmt *Root) {
	mNodeToAdd->addOp(new NativeNodeOp(Root));
	mDirectOut.back().erase(mNodeToAdd);
	mContinueOut.top().insert(mNodeToAdd);
	mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseGotoStmt(GotoStmt *Root) {
	mNodeToAdd->addOp(new NativeNodeOp(Root));
	mGotos.push_back({Root->getLabel()->getStmt(), mNodeToAdd});
	mDirectOut.back().erase(mNodeToAdd);
	mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseLabelStmt(Stmt *Root) {
	mNodeToAdd->addOp(new WrapperNodeOp(Root));
	if (!mPrevFirstLabel.Node || mPrevFirstLabel.Node!=mNodeToAdd ||
    mLastLabelIt<mNodeToAdd->size()-2)
		mPrevFirstLabel={mNodeToAdd, mNodeToAdd->size()-1};
	mLastLabelIt=mNodeToAdd->size()-1;
	mLabels.insert({Root, mPrevFirstLabel});
	if (isa<LabelStmt>(*Root))
		parseStmt(((LabelStmt*)Root)->getSubStmt());
	else
		parseStmt(((SwitchCase*)Root)->getSubStmt());
}

void SourceCFGBuilder::parseReturnStmt(ReturnStmt *Root) {
	if (Root->getRetValue())
		parseExpr(DynTypedNode::create(*Root->getRetValue()),
        new WrapperNodeOp(Root), true);
	else
		mNodeToAdd->addOp(new NativeNodeOp(Root));
	mSCFG->bindNodes(*mNodeToAdd, *mSCFG->getStopNode());
	mDirectOut.back().erase(mNodeToAdd);
	mNodeToAdd=nullptr;
}

void SourceCFGBuilder::parseDeclStmt(DeclStmt *Root, NodeOp *ParentOp) {
	for (auto D : Root->decls())
		if ((D->getKind()==Decl::Kind::Var) && ((VarDecl*)D)->getInit())
			parseExpr(DynTypedNode::create(*D), ParentOp, true);
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
	ClangSourceCFGPrinter() : DOTGraphTraitsPrinterWrapperPass<
    ClangSourceCFGPass, false, tsar::SourceCFG*,
    ClangSourceCFGPassGraphTraits>("scfg", ID) {
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

