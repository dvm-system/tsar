//tsar_fcopy_propagation.cpp - Frontend Copy Propagation (clang) --*- C++ -*-=//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements methods necessary for source-level copy propagation.
///
//===----------------------------------------------------------------------===//


#include "tsar_fcopy_elimination.h"
#include "tsar_transformation.h"
#include "tsar_query.h"

#include <clang/Analysis/CFG.h>
#include <clang/AST/ASTContext.h>
#include <clang/AST/Expr.h>
#include <clang/AST/Stmt.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <clang/Lex/Lexer.h>
#include <llvm/Support/Debug.h>

#include <map>
#include <set>
#include <vector>

// FIXME: only propagates and eliminates copies, dead code elimination
// should be done for meaningless stmts

using namespace clang;
using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "clang-copy-elimination"

char CopyEliminationPass::ID = 0;
INITIALIZE_PASS_IN_GROUP_BEGIN(CopyEliminationPass, "clang-copy-elimination",
  "Source-level copy elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())
INITIALIZE_PASS_DEPENDENCY(TransformationEnginePass)
INITIALIZE_PASS_IN_GROUP_END(CopyEliminationPass, "clang-copy-elimination",
  "Source-level copy elimination (Clang)", false, false,
  TransformationQueryManager::getPassRegistry())

bool DeclRefVisitor::VisitDeclRefExpr(clang::DeclRefExpr* DRE) {
  mDeclRefs[DRE->getDecl()].insert(DRE);
  return true;
}

bool DeclRefVisitor::VisitUnaryOperator(clang::UnaryOperator* UO) {
  mUnaryOps.insert(UO);
  return true;
}

static const clang::Expr* getRHS(const clang::Stmt* S) {
  const clang::Expr* E;
  if (auto BO = dyn_cast<BinaryOperator>(S)) {
    E = BO->getRHS();
  } else if (auto DS = dyn_cast<DeclStmt>(S)) {
    auto VD = dyn_cast<VarDecl>(DS->getSingleDecl());
    E = VD->getInit();
  }
  return E;
}

const clang::Stmt* CopyEliminationPass::isRedefined(const clang::ValueDecl* LHS,
  const clang::ValueDecl* RHS, clang::CFGBlock::iterator B,
  clang::CFGBlock::iterator E) const {
  for (auto I = B; I != E; ++I) {
    if (auto CS = I->getAs<CFGStmt>()) {
      if (auto BO = dyn_cast<BinaryOperator>(CS->getStmt())) {
        if (BO->isAssignmentOp()) {
          auto SLHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParens());
          if (SLHS) {
            if (SLHS->getDecl() == LHS || SLHS->getDecl() == RHS) {
              return CS->getStmt();
            }
          }
        } else if (auto DS = dyn_cast<DeclStmt>(CS->getStmt())) {
          if (auto VD = clang::dyn_cast<VarDecl>(DS->getSingleDecl())) {
            if (VD->hasInit()) {
              if (VD == LHS || VD == RHS) {
                return CS->getStmt();
              } else {
                continue;
              }
            }
          }
        }
      }
    }
  }
  return nullptr;
}

bool CopyEliminationPass::runOnFunction(Function& F) {
  auto M = F.getParent();
  mTfmCtx = getAnalysis<TransformationEnginePass>().getContext(*M);
  if (!mTfmCtx || !mTfmCtx->hasInstance())
    return false;
  auto Decl = mTfmCtx->getDeclForMangledName(F.getName());
  if (!Decl)
    return false;
  mFuncDecl = clang::dyn_cast<clang::FunctionDecl>(Decl);
  if (!mFuncDecl)
    return false;
  mContext = &mTfmCtx->getContext();
  mRewriter = &mTfmCtx->getRewriter();
  mSourceManager = &mContext->getSourceManager();
  auto CFG = clang::CFG::buildCFG(nullptr, mFuncDecl->getBody(), mContext,
    clang::CFG::BuildOptions());
  assert(CFG.get() != nullptr && ("CFG construction failed for "
    + mFuncDecl->getName()).str().data());
  std::vector<std::map<const clang::ValueDecl*, const clang::Stmt*>> Gen(
    CFG->getNumBlockIDs());
  std::vector<std::map<const clang::ValueDecl*, std::set<const clang::Stmt*>>>
    Kill(CFG->getNumBlockIDs());
  std::map<const clang::ValueDecl*, std::set<const clang::Stmt*>> CopyStmts;
  std::map<const clang::ValueDecl*, std::set<const clang::DeclRefExpr*>> Redefs;
  // copy statements - those which have immediate identifier in RHS
  for (auto B : *CFG) {
    auto& GenB = Gen[B->getBlockID()];
    auto& KillB = Kill[B->getBlockID()];
    for (auto I : *B) {
      if (auto CS = I.getAs<CFGStmt>()) {
        if (auto BO = dyn_cast<BinaryOperator>(CS->getStmt())) {
          if (BO->isAssignmentOp()) {
            auto LHS = dyn_cast<DeclRefExpr>(BO->getLHS()->IgnoreParens());
            if (LHS) {
              // assignment to LHS, add to Kill so we know that
              // this variable is killed
              KillB[LHS->getDecl()];
              GenB.erase(LHS->getDecl());
              Redefs[LHS->getDecl()].insert(LHS);
            }
            if (BO->getOpcode() == BO_Assign) {
              // only check that RHS has one token (identifier)
              // and hasn't any unary ops
              DeclRefVisitor DRVisitor;
              DRVisitor.TraverseBinaryOperator(
                const_cast<clang::BinaryOperator*>(BO));
              auto UnaryOps = DRVisitor.getUnaryOps();
              auto Tokens = getRawTokens(getRange(BO->getRHS()));
              auto Text = getSourceText(getRange(BO->getRHS()));
              if (LHS && UnaryOps.empty() && Tokens.size() == 1
                && Text == Tokens[0].getRawIdentifier()) {
                CopyStmts[LHS->getDecl()].insert(CS->getStmt());
                GenB[LHS->getDecl()] = CS->getStmt();
              }
            }
          }
        } else if (auto DS = dyn_cast<DeclStmt>(CS->getStmt())) {
          // CFG generates synthetic single decls instead of real decl groups
          if (auto VD = clang::dyn_cast<VarDecl>(DS->getSingleDecl())) {
            if (VD->hasInit()) {
              auto Tokens = getRawTokens(getRange(VD->getInit()));
              if (Tokens.size() == 1) {
                DeclRefVisitor DRVisitor;
                DRVisitor.TraverseDeclStmt(const_cast<clang::DeclStmt*>(DS));
                auto UnaryOps = DRVisitor.getUnaryOps();
                auto DeclRefs = DRVisitor.getDeclRefs();
                auto TokenReferenced
                  = [&](const std::pair<const clang::ValueDecl*,
                    std::set<const clang::DeclRefExpr*>> DeclRefs) -> bool {
                  return DeclRefs.first->getName()
                    == Tokens[0].getRawIdentifier();
                };
                assert(std::find_if(std::begin(DeclRefs), std::end(DeclRefs),
                  TokenReferenced) != std::end(DeclRefs)
                  && "Raw identifier: found in source, not found in AST");
                auto Text = getSourceText(getRange(VD->getInit()));
                if (UnaryOps.empty() && Text == Tokens[0].getRawIdentifier()) {
                  CopyStmts[VD].insert(CS->getStmt());
                  GenB[VD] = CS->getStmt();
                }
              }
            }
          }
        } else if (auto UO = dyn_cast<UnaryOperator>(CS->getStmt())) {
          if (UO->isIncrementDecrementOp()) {
            auto LHS = dyn_cast<DeclRefExpr>(UO->getSubExpr());
            if (LHS) {
              // assignment to LHS, add to Kill so we know that
              // this variable is killed
              KillB[LHS->getDecl()];
              GenB.erase(LHS->getDecl());
              Redefs[LHS->getDecl()].insert(LHS);
            }
          }
        }
      }
    }
  }
  // complete Kill sets with statements
  for (auto B : *CFG) {
    auto& GenB = Gen[B->getBlockID()];
    auto& KillB = Kill[B->getBlockID()];
    for (auto& Kill : KillB) {
      if (!Kill.second.empty()) {
        continue;
      }
      // kill any previous copy statements, having LHS
      // on any side
      for (auto& CopyStmt : CopyStmts) {
        for (auto S : CopyStmt.second) {
          auto Tokens = getRawTokens(getRange(getRHS(S)));
          assert(Tokens.size() == 1);
          DeclRefVisitor DRVisitor;
          DRVisitor.TraverseStmt(const_cast<Stmt*>(S));
          auto DeclRefs = DRVisitor.getDeclRefs();
          if ((DeclRefs.find(Kill.first) != std::end(DeclRefs)
            && Tokens[0].getRawIdentifier() == Kill.first->getName())
              || Kill.first == CopyStmt.first) {
            KillB[CopyStmt.first].insert(S);
          }
        }
      }
    }
    for (auto it = std::begin(KillB); it != std::end(KillB);) {
      if (it->second.empty()) {
        it = KillB.erase(it);
      } else {
        ++it;
      }
    }
    for (auto& Gen : GenB) {
      if (KillB.find(Gen.first) != std::end(KillB)) {
        KillB[Gen.first].erase(Gen.second);
        if (KillB[Gen.first].empty()) {
          KillB.erase(Gen.first);
        }
      }
    }
  }

  DEBUG(
  for (auto B : *CFG) {
    B->dump();
    llvm::dbgs() << "GEN(B" << B->getBlockID() << "):" << '\n';
    for (auto& GenB : Gen[B->getBlockID()]) {
      llvm::dbgs() << "  " << GenB.first << ' ' << GenB.first->getName() << ' '
        << getSourceText(getRange(GenB.second)) << ':' << '\n';
    }
    llvm::dbgs() << "KILL(B" << B->getBlockID() << "):" << '\n';
    for (auto& KillB : Kill[B->getBlockID()]) {
      llvm::dbgs() << "  " << KillB.first << ' ' << KillB.first->getName()
        << ':' << '\n';
      for (auto S : KillB.second) {
        llvm::dbgs() << "    " << getSourceText(getRange(S)) << '\n';
      }
    }
  }
  );

  mIn = std::vector<std::map<const clang::ValueDecl*,
    std::set<const clang::Stmt*>>>(CFG->getNumBlockIDs());
  mOut = std::vector<std::map<const clang::ValueDecl*,
    std::set<const clang::Stmt*>>>(CFG->getNumBlockIDs());
  for (auto B : *CFG) {
    if (B->getBlockID() != CFG->getEntry().getBlockID()) {
      mOut[B->getBlockID()] = CopyStmts;
    }
  }
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto B : *CFG) {
      if (B == &CFG->getEntry()) {
        continue;
      }
      auto& GenB = Gen[B->getBlockID()];
      auto& KillB = Kill[B->getBlockID()];
      auto& InB = mIn[B->getBlockID()];
      auto& OutB = mOut[B->getBlockID()];

      auto NewInB = CopyStmts;
      for (auto PB : B->preds()) {
        if (!PB) {
          continue;
        }
        for (auto& OutB : mOut[PB->getBlockID()]) {
          if (NewInB.find(OutB.first) != std::end(NewInB)) {
            std::set<const clang::Stmt*> Intersection;
            std::set_intersection(
              std::begin(NewInB[OutB.first]), std::end(NewInB[OutB.first]),
              std::begin(OutB.second), std::end(OutB.second),
              std::inserter(Intersection, std::end(Intersection)));
            if (Intersection.empty()) {
              NewInB.erase(OutB.first);
            } else {
              NewInB[OutB.first] = Intersection;
            }
          }
        }
        for (auto it = std::begin(NewInB); it != std::end(NewInB);) {
          if (mOut[PB->getBlockID()].find(it->first)
            == std::end(mOut[PB->getBlockID()])) {
            it = NewInB.erase(it);
          } else {
            ++it;
          }
        }
      }
      InB.swap(NewInB);

      auto NewOutB = InB;
      for (auto& Kill : KillB) {
        if (NewOutB.find(Kill.first) != std::end(NewOutB)) {
          std::set<const clang::Stmt*> Difference;
          std::set_difference(
            std::begin(NewOutB[Kill.first]), std::end(NewOutB[Kill.first]),
            std::begin(Kill.second), std::end(Kill.second),
            std::inserter(Difference, std::end(Difference)));
          if (Difference.empty()) {
            NewOutB.erase(Kill.first);
          } else {
            assert(Difference.size() == 1
              && "Block can't have multiple outputs per identifier");
            NewOutB[Kill.first] = Difference;
          }
        }
      }
      for (auto& Gen : GenB) {
        NewOutB[Gen.first].insert(Gen.second);
      }

      if (NewOutB != OutB) {
        Changed = true;
        OutB.swap(NewOutB);
      }
    }
  }

  DEBUG(
  for (auto B : *CFG) {
    B->dump();
    llvm::dbgs() << "GEN(B" << B->getBlockID() << "):" << '\n';
    llvm::dbgs() << "IN(B" << B->getBlockID() << "):" << '\n';
    for (auto& InB : mIn[B->getBlockID()]) {
      llvm::dbgs() << "  " << InB.first << ' ' << InB.first->getName() << ':'
        << '\n';
      for (auto S : InB.second) {
        llvm::dbgs() << "    " << getSourceText(getRange(S)) << '\n';
      }
    }
    llvm::dbgs() << "OUT:" << '\n';
    for (auto& OutB : mOut[B->getBlockID()]) {
      llvm::dbgs() << "  " << OutB.first << ' ' << OutB.first->getName() << ':'
        << '\n';
      for (auto S : OutB.second) {
        llvm::dbgs() << "    " << getSourceText(getRange(S)) << '\n';
      }
    }
  }
  );

  auto getBlocksWithIn = [&](const clang::ValueDecl* LHS, const clang::Stmt* S) {
    std::set<unsigned int> Blocks;
    for (unsigned int i = 0; i < mIn.size(); ++i) {
      if (mIn[i].find(LHS) != std::end(mIn[i])) {
        if (mIn[i][LHS].find(S) != std::end(mIn[i][LHS])) {
          Blocks.insert(i);
        }
      }
    }
    return Blocks;
  };
  auto getStmtBlock = [&](const clang::Stmt* S) -> CFGBlock* {
    for (auto B : *CFG) {
      for (auto I : *B) {
        if (auto CS = I.getAs<CFGStmt>()) {
          if (CS->getStmt() == S) {
            return B;
          }
        }
      }
    }
    assert("Statement not found in CFG");
    return nullptr;
  };
  auto rewriteRange = [&](const clang::ValueDecl* LHS,
    const clang::ValueDecl* RHS, clang::CFGBlock::iterator B,
    clang::CFGBlock::iterator E) {
    std::map<const clang::ValueDecl*, std::set<const clang::DeclRefExpr*>>
      RewrittenDecls;
    auto RedefStmt = isRedefined(LHS, RHS, B, E);
    for (auto I = B; I != E; ++I) {
      if (auto CS = I->getAs<CFGStmt>()) {
        if (CS->getStmt() == RedefStmt) {
          break;
        }
        DeclRefVisitor DRVisitor;
        DRVisitor.TraverseStmt(const_cast<Stmt*>(CS->getStmt()));
        auto DeclRefs = DRVisitor.getDeclRefs()[LHS];
        RewrittenDecls[LHS].insert(std::begin(DeclRefs),
          std::end(DeclRefs));
        for (auto DRE : DeclRefs) {
          // rewrite DRE with RHS
          DEBUG(
          llvm::dbgs() << getSourceText(getRange(CS->getStmt())) << ':'
            << LHS->getName() << " -> " << RHS->getName() << '\n';
          );
          mRewriter->ReplaceText(getRange(DRE), RHS->getName());
        }
      }
    }
    return RewrittenDecls;
  };

  std::map<const clang::ValueDecl*, std::set<const clang::DeclRefExpr*>>
    RewrittenDecls;
  DeclRefVisitor DRVisitor;
  DRVisitor.TraverseTranslationUnitDecl(mContext->getTranslationUnitDecl());
  auto AllDeclRefs = DRVisitor.getDeclRefs();
  for (auto& CopyStmt : CopyStmts) {
    auto LHS = CopyStmt.first;
    auto VDCopyStmts = CopyStmt.second;
    for (auto S : VDCopyStmts) {
      auto Tokens = getRawTokens(getRange(getRHS(S)));
      assert(Tokens.size() == 1);
      DeclRefVisitor DRVisitor;
      DRVisitor.TraverseStmt(const_cast<Stmt*>(S));
      auto DeclRefs = DRVisitor.getDeclRefs();
      auto TokenReferenced
        = [&](const std::pair<const clang::ValueDecl*,
          std::set<const clang::DeclRefExpr*>> DeclRefs) -> bool {
        return DeclRefs.first->getName()
          == Tokens[0].getRawIdentifier();
      };
      auto it = std::find_if(std::begin(DeclRefs), std::end(DeclRefs),
        TokenReferenced);
      assert(it != std::end(DeclRefs)
        && "Raw identifier: found in source, not found in AST");
      auto RHS = it->first;
      // handle blocks reached by this copy stmt
      auto BlockIDs = getBlocksWithIn(LHS, S);
      for (auto B : *CFG) {
        if (BlockIDs.find(B->getBlockID()) == std::end(BlockIDs)) {
          continue;
        }
        auto BlockRewrittenDecls = rewriteRange(LHS, RHS, B->begin(), B->end());
        for (auto& Decl : BlockRewrittenDecls) {
          RewrittenDecls[Decl.first].insert(std::begin(Decl.second),
            std::end(Decl.second));
        }
      }
      // handle block containing this copy stmt
      auto B = getStmtBlock(S);
      for (auto I = B->begin(); I != B->end(); ++I) {
        if (auto CS = I->getAs<CFGStmt>()) {
          if (CS->getStmt() != S) {
            continue;
          } else {
            auto BlockRewrittenDecls = rewriteRange(LHS, RHS, I + 1, B->end());
            for (auto& Decl : BlockRewrittenDecls) {
              RewrittenDecls[Decl.first].insert(std::begin(Decl.second),
                std::end(Decl.second));
            }
            break;
          }
        }
      }
    }
  }
  for (auto& Decl : RewrittenDecls) {
    auto DeclRefs = AllDeclRefs[Decl.first];
    auto& DeclRedefs = Redefs[Decl.first];
    auto& RewrittenRefs = Decl.second;
    std::set<const clang::DeclRefExpr*> Difference;
    std::set_difference(std::begin(DeclRefs), std::end(DeclRefs),
      std::begin(DeclRedefs), std::end(DeclRedefs),
      std::inserter(Difference, std::end(Difference)));
    Difference.swap(DeclRefs);
    Difference.clear();
    std::set_difference(std::begin(DeclRefs), std::end(DeclRefs),
      std::begin(RewrittenRefs), std::end(RewrittenRefs),
      std::inserter(Difference, std::end(Difference)));
    if (Difference.empty()) {
      DEBUG(
      llvm::dbgs() << getSourceText(getRange(Decl.first)) << " : "
        << Decl.first->getName() << " can be omitted" << '\n';
      );
    }
  }
  return false;
}

std::vector<clang::Token> CopyEliminationPass::getRawTokens(
  const clang::SourceRange& SR) const {
  // these positions are beginings of tokens
  // should include upper bound to capture last token
  unsigned int Offset = SR.getBegin().getRawEncoding();
  unsigned int Length = Offset
    + (getSourceText(SR).size() > 0 ? getSourceText(SR).size() - 1 : 0);
  std::vector<clang::Token> Tokens;
  for (unsigned int Pos = Offset; Pos <= Length;) {
    clang::SourceLocation Loc;
    clang::Token Token;
    Loc = clang::Lexer::GetBeginningOfToken(Loc.getFromRawEncoding(Pos),
      *mSourceManager, mContext->getLangOpts());
    if (clang::Lexer::getRawToken(Loc, Token, *mSourceManager,
      mContext->getLangOpts(), false)) {
      ++Pos;
      continue;
    }
    if (Token.getKind() != clang::tok::raw_identifier) {
      Pos += std::max(1u, (Token.isAnnotation() ? 1u : Token.getLength()));
      continue;
    }
    // avoid duplicates for same token
    if (Tokens.empty()
      || Tokens[Tokens.size() - 1].getLocation() != Token.getLocation()) {
      Tokens.push_back(Token);
    }
    Pos += Token.getLength();
  }
  return Tokens;
}

std::string CopyEliminationPass::getSourceText(
  const clang::SourceRange& SR) const {
  return clang::Lexer::getSourceText(clang::CharSourceRange::getTokenRange(SR),
    *mSourceManager, mContext->getLangOpts());
}

template<typename T>
clang::SourceRange CopyEliminationPass::getRange(T* node) const {
  return {mSourceManager->getFileLoc(node->getSourceRange().getBegin()),
    mSourceManager->getFileLoc(node->getSourceRange().getEnd())};
}

void CopyEliminationPass::getAnalysisUsage(AnalysisUsage& AU) const {
  AU.addRequired<TransformationEnginePass>();
  AU.setPreservesAll();
}

FunctionPass* createCopyEliminationPass() {
  return new CopyEliminationPass();
}
