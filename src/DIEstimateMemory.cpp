//===- DIEstimateMemory.cpp - Memory Hierarchy (Debug) ----------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
//===----------------------------------------------------------------------===//
//
// This file proposes functionality to construct a program alias tree.
//
//===----------------------------------------------------------------------===//

#include "DIEstimateMemory.h"
#include "tsar_dbg_output.h"
#include <llvm/ADT/Statistic.h>
#include <llvm/Analysis/MemoryLocation.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/InstIterator.h>

using namespace llvm;
using namespace tsar;

#undef DEBUG_TYPE
#define DEBUG_TYPE "di-estimate-mem"

STATISTIC(NumAliasNode, "Number of alias nodes created");
STATISTIC(NumEstimateNode, "Number of estimate nodes created");
STATISTIC(NumUnknownNode, "Number of unknown nodes created");
STATISTIC(NumEstimateMemory, "Number of estimate memory created");
STATISTIC(NumUnknownMemory, "Number of unknown memory created");

namespace tsar {
bool mayAliasFragments(const DIExpression &LHS, const DIExpression &RHS) {
  if (LHS.getNumElements() != 3 || RHS.getNumElements() != 3)
    return true;
  auto LHSFragment = LHS.getFragmentInfo();
  auto RHSFragment = RHS.getFragmentInfo();
  if (!LHSFragment || !RHSFragment)
    return true;
  if (LHSFragment->SizeInBits == 0 || RHSFragment->SizeInBits == 0)
    return false;
  return ((LHSFragment->OffsetInBits == RHSFragment->OffsetInBits) ||
          (LHSFragment->OffsetInBits < RHSFragment->OffsetInBits &&
          LHSFragment->OffsetInBits + LHSFragment->SizeInBits >
            RHSFragment->OffsetInBits) ||
          (RHSFragment->OffsetInBits < LHSFragment->OffsetInBits &&
          RHSFragment->OffsetInBits + RHSFragment->SizeInBits >
            LHSFragment->OffsetInBits));
}
}

std::unique_ptr<DIEstimateMemory> DIEstimateMemory::get(llvm::LLVMContext &Ctx,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  auto MD = llvm::MDNode::get(Ctx, { Var, Expr, FlagMD });
  assert(MD && "Can not create metadata node!");
  return std::unique_ptr<DIEstimateMemory>(new DIEstimateMemory(MD));
}

std::unique_ptr<DIEstimateMemory>
DIEstimateMemory::getIfExists(llvm::LLVMContext &Ctx,
    llvm::DIVariable *Var, llvm::DIExpression *Expr, Flags F) {
  assert(Var && "Variable must not be null!");
  assert(Expr && "Expression must not be null!");
  auto *FlagMD = llvm::ConstantAsMetadata::get(
    llvm::ConstantInt::get(Type::getInt16Ty(Ctx), F));
  if (auto MD = llvm::MDNode::getIfExists(Ctx, { Var, Expr, FlagMD }))
    return std::unique_ptr<DIEstimateMemory>(new DIEstimateMemory(MD));
  return nullptr;
}
llvm::DIVariable * DIEstimateMemory::getVariable() {
  for (unsigned I = 0, EI = mMD->getNumOperands(); I < EI; ++I)
    if (auto *Var = llvm::dyn_cast<llvm::DIVariable>(mMD->getOperand(I)))
      return Var;
  llvm_unreachable("Variable must be specified!");
  return nullptr;
}

const llvm::DIVariable * DIEstimateMemory::getVariable() const {
  for (unsigned I = 0, EI = mMD->getNumOperands(); I < EI; ++I)
    if (auto *Var = llvm::dyn_cast<llvm::DIVariable>(mMD->getOperand(I)))
      return Var;
  llvm_unreachable("Variable must be specified!");
  return nullptr;
}

llvm::DIExpression * DIEstimateMemory::getExpression() {
  for (unsigned I = 0, EI = mMD->getNumOperands(); I < EI; ++I)
    if (auto *Expr = llvm::dyn_cast<llvm::DIExpression>(mMD->getOperand(I)))
      return Expr;
  llvm_unreachable("Expression must be specified!");
  return nullptr;
}

const llvm::DIExpression * DIEstimateMemory::getExpression() const {
  for (unsigned I = 0, EI = mMD->getNumOperands(); I < EI; ++I)
    if (auto *Expr = llvm::dyn_cast<llvm::DIExpression>(mMD->getOperand(I)))
      return Expr;
  llvm_unreachable("Expression must be specified!");
  return nullptr;
}

unsigned DIEstimateMemory::getFlagsOp() const {
  for (unsigned I = 0, EI = mMD->getNumOperands(); I < EI; ++I) {
    auto &Op = mMD->getOperand(I);
    if (isa<DIVariable>(Op) || isa<DIExpression>(Op))
      continue;
    auto CMD = dyn_cast<ConstantAsMetadata>(Op);
    if (!CMD)
      continue;
    if (auto CInt = dyn_cast<ConstantInt>(CMD->getValue()))
      return I;
  }
  llvm_unreachable("Explicit flag must be specified!");
}

DIEstimateMemory::Flags DIEstimateMemory::getFlags() const {
  auto CMD = cast<ConstantAsMetadata>(mMD->getOperand(getFlagsOp()));
  auto CInt = cast<ConstantInt>(CMD->getValue());
  return static_cast<Flags>(CInt->getZExtValue());
}

void DIEstimateMemory::setFlags(Flags F) {
  auto OpIdx = getFlagsOp();
  auto CMD = cast<ConstantAsMetadata>(mMD->getOperand(OpIdx));
  auto CInt = cast<ConstantInt>(CMD->getValue());
  auto &Ctx = mMD->getContext();
  auto *FlagMD = llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
   Type::getInt16Ty(Ctx), static_cast<Flags>(CInt->getZExtValue()) | F));
    mMD->replaceOperandWith(OpIdx, FlagMD);
}

namespace {
/// This class inserts new subtree into an alias tree.
///
/// The subtree will contains a list of specified fragments of a specified
/// variable. This fragments should not alias with other memory location in the
/// alias tree. This is a precondition and is not checked by a builder.
/// It also tries to build fragments which extends specified fragments.
/// For example for the fragment S.X[2] the following locations will be
/// constructed S -> S.X -> S.X[2]. However, the last one only will be marked as
/// explicitly accessed memory location.
class DIAliasTreeBuilder {
public:
  /// Creates a builder of subtree which contains specified fragments of a
  /// variable.
  DIAliasTreeBuilder(DIAliasTree *AT, LLVMContext &Ctx, DIVariable *Var,
      const TinyPtrVector<DIExpression *> Fragments) :
      mAliasTree(AT), mContext(&Ctx), mVar(Var), mSortedFragments(Fragments) {
    assert(mAliasTree && "Alias tree must not be null!");
    assert(mVar && "Variable must not be null!");
    assert(!Fragments.empty() && "At least one fragment must be specified!");
    std::sort(mSortedFragments.begin(), mSortedFragments.end(),
      [this](DIExpression *LHS, DIExpression *RHS) {
        assert(!mayAliasFragments(*LHS, *RHS) && "Fragments must not be alias!");
        auto InfoLHS = LHS->getFragmentInfo();
        auto InfoRHS = RHS->getFragmentInfo();
        // Empty expression is no possible here because the whole location is
        // alias with any fragment. In this case list of fragments will contain
        // only single expression and this function will not be called.
        assert(LHS->getNumElements() == 3 && InfoLHS.hasValue() &&
          "Expression may contain dwarf::DW_OP_LLVM_fragment only!");
        assert(RHS->getNumElements() == 3 && InfoRHS.hasValue() &&
          "Expression may contain dwarf::DW_OP_LLVM_fragment only!");
        return InfoLHS->OffsetInBits < InfoRHS->OffsetInBits;
    });
  }

  /// Builds a subtree of an alias tree.
  void buildSubtree() {
    auto Ty = stripDIType(mVar->getType()).resolve();
    if (!Ty) {
      addFragments(*mAliasTree->getTopLevelNode(), 0, mSortedFragments.size());
      return;
    }
    if (mSortedFragments.front()->getNumElements() == 0) {
      mAliasTree->addNewNode(
        DIEstimateMemory::get(
          *mContext, mVar, mSortedFragments.front(), DIEstimateMemory::Explicit),
        *mAliasTree->getTopLevelNode());
      return;
    }
    auto LastInfo = mSortedFragments.back()->getFragmentInfo();
    if (LastInfo->OffsetInBits / 8 + (LastInfo->SizeInBits + 7) / 8 >
        getSize(Ty)) {
      addFragments(*mAliasTree->getTopLevelNode(), 0, mSortedFragments.size());
      return;
    }
    evaluateTy(Ty, 0, std::make_pair(0, mSortedFragments.size()),
      mAliasTree->getTopLevelNode());
  }

private:
  /// \brief Add fragments from a specified range [BeginIdx, EndIdx)
  /// into an alias tree.
  ///
  /// Each fragments will be stored in a separate node with a parent `Parent`.
  void addFragments(DIAliasNode &Parent, unsigned BeginIdx, unsigned EndIdx) {
    for (unsigned I = BeginIdx; I < EndIdx; ++I) {
      mAliasTree->addNewNode(
        DIEstimateMemory::get(
          *mContext, mVar, mSortedFragments[I], DIEstimateMemory::Explicit),
        Parent);
    }
  }

  /// \brief Add subtypes of a specified type into the alias tree.
  /// the alias tree.
  ///
  /// \pre A specified type `Ty` must full cover a specified fragments
  /// from range [Fragments.first, Fragments.second).
  /// Fragments from this range can not cross bounds of this type.
  void evaluateTy(DIType *Ty, uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent) {
    assert(Ty && "Type must not be null!");
    assert(Parent && "Alias node must not be null!");
    auto Size = getSize(Ty);
    auto FInfo = mSortedFragments[Fragments.first]->getFragmentInfo();
    if (Fragments.first + 1 == Fragments.second &&
        FInfo->OffsetInBits / 8 == Offset &&
        (FInfo->SizeInBits + 7) / 8 == Size) {
      addFragments(*Parent, Fragments.first, Fragments.second);
      return;
    }
    auto Expr = DIExpression::get(*mContext, {
      dwarf::DW_OP_LLVM_fragment, Offset, Ty->getSizeInBits()});
    auto &EM = mAliasTree->addNewNode(
      DIEstimateMemory::get(*mContext, mVar, Expr), *Parent);
    Parent = EM.getAliasNode();
    switch (Ty->getTag()) {
    default:
      addFragments(*Parent, Fragments.first, Fragments.second);
      break;
    case dwarf::DW_TAG_structure_type:
    case dwarf::DW_TAG_class_type:
      evaluateStructureTy(Ty, Offset, Fragments, Parent);
      break;
    case dwarf::DW_TAG_array_type:
      evaluateArrayTy(Ty, Offset, Fragments, Parent);
      break;
    }
  }

  /// \brief Build coverage of fragments from a specified range
  /// [Fragments.first, Fragments.second) and update alias tree.
  ///
  /// The range of elements will be spitted into subranges. Each subrange
  /// is covered by some subsequence of elements.
  /// Adds a specified element into alias tree if it full covers some fragments
  /// from a specified range and evaluates its base type recursively.
  /// If fragments cross border of elements before the current one then elements
  /// will not be added into the alias tree. In this case only fragments will
  /// be inserted after full coverage will be constructed (sequence of elements
  /// which full covers a subrange of fragments).
  /// \param [in, out] FragmentIdx Index of a fragment in the range. It will be
  /// increased after function call.
  /// \param [in, out] FirstElIdx Index of a first element in a coverage which
  /// is under construction.
  /// \param [in, out] FirstFragmentIdx Index of a first fragment in a range
  /// which is not covered yet.
  void evaluateElement(uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent,
      DIType *ElTy, unsigned ElIdx, uint64_t ElOffset, uint64_t ElSize,
      unsigned &FirstElIdx, unsigned &FirstFragmentIdx, unsigned &FragmentIdx) {
    bool IsFragmentsCoverage = false;
    for (; FragmentIdx < Fragments.second;
         ++FragmentIdx, IsFragmentsCoverage = true) {
      auto FInfo = mSortedFragments[FragmentIdx]->getFragmentInfo();
      auto FOffset = FInfo->OffsetInBits / 8;
      auto FSize = (FInfo->SizeInBits + 7) / 8;
      // If condition is true than elements in [FirstElIdx, ElIdx] cover
      // fragments in [mFirstFragment, FragmentIdx).
      if (FOffset >= Offset + ElOffset + ElSize)
        break;
      // If condition is true than FragmentIdx cross the border of ElIdx and
      // it is necessary include the next element in coverage.
      if (FOffset + FSize > Offset + ElOffset + ElSize) {
        IsFragmentsCoverage = false;
        break;
      }
    }
    if (!IsFragmentsCoverage)
      return;
    if (FirstElIdx == ElIdx) {
      evaluateTy(ElTy, Offset + ElOffset,
        std::make_pair(FirstFragmentIdx, FragmentIdx), Parent);
    } else {
      // A single element of a aggregate type does not cover set of fragments.
      // In this case elements that comprises a coverage will not be added
      // into alias tree. Instead only fragments will be inserted.
      addFragments(*Parent, FirstFragmentIdx, FragmentIdx);
    }
    FirstElIdx = ElIdx + 1;
    FirstFragmentIdx = FragmentIdx;
  }

  /// \brief Splits array type into its elements and adds them into
  /// the alias tree.
  ///
  /// \pre A specified type `Ty` must full cover a specified fragments
  /// from range [Fragments.first, Fragments.second).
  /// Fragments from this range can not cross bounds of this type.
  void evaluateArrayTy(DIType *Ty, uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent) {
    assert(Ty && Ty->getTag() == dwarf::DW_TAG_array_type &&
      "Type to evaluate must be a structure or class!");
    assert(Parent && "Alias node must not be null!");
    auto DICTy = cast<DICompositeType>(Ty);
    auto ElTy = stripDIType(DICTy->getBaseType()).resolve();
    auto ElSize = getSize(ElTy);
    unsigned FirstElIdx = 0;
    unsigned ElIdx = 0;
    auto FirstFragmentIdx = Fragments.first;
    auto FragmentIdx = Fragments.first;
    for (uint64_t ElOffset = 0, E = DICTy->getSizeInBits();
         ElOffset < E && FragmentIdx < Fragments.second;
         ElOffset += ElSize, ElIdx++) {
      evaluateElement(Offset, Fragments, Parent, ElTy,
        ElIdx, ElOffset, ElSize, FirstElIdx, FirstFragmentIdx, FragmentIdx);
    }
    addFragments(*Parent, FirstFragmentIdx, FragmentIdx);
  }

  /// \brief Splits structure type into its elements and adds them into
  /// the alias tree.
  ///
  /// \pre A specified type `Ty` must full cover a specified fragments
  /// from range [Fragments.first, Fragments.second).
  /// Fragments from this range can not cross bounds of this type.
  void evaluateStructureTy(DIType *Ty, uint64_t Offset,
      std::pair<unsigned, unsigned> Fragments, DIAliasNode *Parent) {
    assert(Ty &&
      (Ty->getTag() == dwarf::DW_TAG_structure_type ||
       Ty->getTag() == dwarf::DW_TAG_class_type) &&
      "Type to evaluate must be a structure or class!");
    assert(Parent && "Alias node must not be null!");
    auto DICTy = cast<DICompositeType>(Ty);
    auto FirstFragmentIdx = Fragments.first;
    auto FragmentIdx = Fragments.first;
    unsigned  FirstElIdx = 0;
    for (unsigned ElIdx = 0, ElEnd = DICTy->getElements().size();
         ElIdx < ElEnd && FragmentIdx < Fragments.second; ++ElIdx) {
      auto ElTy = cast<DIDerivedType>(
        stripDIType(cast<DIType>(DICTy->getElements()[ElIdx])));
      auto ElOffset = ElTy->getOffsetInBits() / 8;
      auto ElSize = getSize(ElTy);
      evaluateElement(Offset, Fragments, Parent, ElTy->getBaseType().resolve(),
        ElIdx, ElOffset, ElSize, FirstElIdx, FirstFragmentIdx, FragmentIdx);
    }
    addFragments(*Parent, FirstFragmentIdx, FragmentIdx);
  }

  DIAliasTree *mAliasTree;
  LLVMContext *mContext;
  DIVariable *mVar;
  TinyPtrVector<DIExpression *> mSortedFragments;
};
}

char DIEstimateMemoryPass::ID = 0;
INITIALIZE_PASS_BEGIN(DIEstimateMemoryPass, "di-estimate-mem",
  "Memory Estimator (Debug)", false, true)
INITIALIZE_PASS_END(DIEstimateMemoryPass, "di-estimate-mem",
  "Memory Estimator (Debug)", false, true)

void DIEstimateMemoryPass::getAnalysisUsage(AnalysisUsage & AU) const {
  AU.setPreservesAll();
}

FunctionPass * llvm::createDIEstimateMemoryPass() {
  return new DIEstimateMemoryPass();
}

bool DIEstimateMemoryPass::runOnFunction(Function &F) {
  releaseMemory();
  mContext = &F.getContext();
  mAliasTree = new DIAliasTree;
  DIFragmentMap VarToFragment;
  DIMemorySet SmallestFragments;
  findNoAliasFragments(F, VarToFragment, SmallestFragments);
  F.getParent()->dump();
  for (auto &VToF : VarToFragment) {
    if (VToF.get<DIExpression>().empty())
      continue;
    DIAliasTreeBuilder Builder(mAliasTree, F.getContext(),
      VToF.get<DIVariable>(), VToF.get<DIExpression>());
    Builder.buildSubtree();
  }
  return false;
}

void DIEstimateMemoryPass::findNoAliasFragments(
    Function &F, DIFragmentMap &VarToFragment, DIMemorySet &SmallestFragments) {
  for (auto &I : make_range(inst_begin(F), inst_end(F))) {
    if (!isa<DbgValueInst>(I))
      continue;
    auto Loc = DIMemoryLocation::get(&I);
    if (SmallestFragments.count(Loc))
      continue;
    if (auto *V = MetadataAsValue::getIfExists(I.getContext(), Loc.Var)) {
      // If llvm.dbg.declare exists it means that alloca has not been fully
      // promoted to registers, so we can not analyze it here due to
      // this pass ignores alias analysis results.
      //
      /// TODO (kaniandr@gmail.com): It is planed to deprecate
      /// llvm.dbg.declare intrinsic in the future releases. This check
      /// should be changed if this occurs.
      bool HasDbgDeclare = false;
      for (User *U : V->users())
        if (HasDbgDeclare =
              (isa<DbgDeclareInst>(U) &&
              !isa<UndefValue>(cast<DbgDeclareInst>(U)->getAddress())))
          break;
      if (HasDbgDeclare)
        continue;
    }
    auto VarFragments = VarToFragment.try_emplace(Loc.Var, Loc.Expr);
    if (VarFragments.second) {
      SmallestFragments.insert(std::move(Loc));
      continue;
    }
    // Empty list of fragments means that this variable should be ignore due
    // to some reason, for example, existence of llvm.dbg.declare.
    // The reason has been discovered on the previous iterations.
    if (VarFragments.first->get<DIExpression>().empty())
        continue;
    if ([this, &Loc, &VarFragments, &SmallestFragments]() {
      for (auto *Expr : VarFragments.first->get<DIExpression>()) {
        if (mayAliasFragments(*Expr, *Loc.Expr)) {
          for (auto *EraseExpr : VarFragments.first->get<DIExpression>())
            SmallestFragments.erase(DIMemoryLocation{ Loc.Var, EraseExpr });
          VarFragments.first->get<DIExpression>().clear();
          return true;
        }
      }
      return false;
    }())
      continue;
    VarFragments.first->get<DIExpression>().push_back(Loc.Expr);
    SmallestFragments.insert(std::move(Loc));
  }
}
