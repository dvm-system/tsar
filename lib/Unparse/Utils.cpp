//===- Utils.cpp ------------ Output Functions ------------------*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2018 DVM System Group
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
//
// This file implements a set of output functions for various bits of
// information.
//
//===----------------------------------------------------------------------===//

#include "tsar/Unparse/Utils.h"
#include "tsar/Analysis/Memory/DIEstimateMemory.h"
#include "tsar/Analysis/Memory/DIMemoryLocation.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/Memory/MemoryLocationRange.h"
#include "tsar/Unparse/DIUnparser.h"
#include "tsar/Unparse/SourceUnparserUtils.h"
#include "tsar/Unparse/VariableLocation.h"
#include <llvm/IR/DebugInfo.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Transforms/Utils/Local.h>

using namespace llvm;

namespace tsar {
void printLocationSource(llvm::raw_ostream &O, const Value *Loc,
    const DominatorTree *DT) {
  if (!Loc)
    O << "?";
  else if (!unparsePrint(O, Loc, DT))
    Loc->printAsOperand(O, false);
}

void printLocationSource(llvm::raw_ostream &O, const llvm::MemoryLocation &Loc,
    const DominatorTree *DT) {
  O << "<";
  printLocationSource(O, Loc.Ptr, DT);
  O << ", ";
  if (!Loc.Size.hasValue())
    if (Loc.Size.mayBeBeforePointer())
      O << "?";
    else
      O << "+?";
  else
    O << Loc.Size.getValue();
  O << ">";
}

void printLocationSource(llvm::raw_ostream &O, const MemoryLocationRange &Loc,
    const DominatorTree *DT, bool IsDebug) {
  O << "<";
  printLocationSource(O, Loc.Ptr, DT);
  O << ", ";
  if (!Loc.LowerBound.hasValue())
    if (Loc.LowerBound.mayBeBeforePointer())
      O << "?";
    else
      O << "+?";
  else
    O << Loc.LowerBound.getValue();
  O << ", ";
  if (!Loc.UpperBound.hasValue())
    if (Loc.UpperBound.mayBeBeforePointer())
      O << "?";
    else
      O << "+?";
  else
    O << Loc.UpperBound.getValue();
  if (!IsDebug) {
    if (!Loc.DimList.empty()) {
      O << ", ";
      for (auto &Dim : Loc.DimList) {
        O << "[";
        O << Dim.Start << ":" << Dim.TripCount << ":" << Dim.Step << "," <<
             Dim.DimSize;
        O << "]";
      }
    }
  }
  O << ">";
  if (IsDebug) {
    if (!Loc.DimList.empty()) {
      O << ", {";
      for (auto &Dimension : Loc.DimList)
        O << "{Start: " << Dimension.Start << ", Step: " << Dimension.Step <<
            ", TripCount: " << Dimension.TripCount << ", DimSize: " <<
            Dimension.DimSize << "}";
      O << "}";
    }
    O << " (" << Loc.getKindAsString() << ") ";
    O << " [" << Loc.Ptr << "]";
  }
}
void printLocationSource(llvm::raw_ostream &O, const EstimateMemory &EM,
    const DominatorTree *DT) {
  printLocationSource(O, MemoryLocation(EM.front(), EM.getSize()), DT);
}

void printDILocationSource(unsigned DWLang,
    const DIMemoryLocation &Loc, raw_ostream &O) {
  if (!Loc.isValid()) {
    O << "<";
    O << "sapfor.invalid";
    if (Loc.Var)
      O << "(" << Loc.Var->getName() << ")";
    O << ",?>";
    return;
  }
  O << "<";
  if (!unparsePrint(DWLang, Loc, O))
    O << "?" << Loc.Var->getName() << "?";
  O << ", ";
  auto Size = Loc.getSize();
  if (!Size.hasValue())
    if (Size.mayBeBeforePointer())
      O << "?";
    else
      O << "+?";
  else
    O << Size.getValue();
  O << ">";
}

template<class PositionT>
static bool printAt(const PositionT  *At, const Twine &Prefix, raw_ostream &O) {
  if (At && At->getLine() > 0) {
    O << Prefix << At->getLine();
    if (At->getColumn())
      O << ":" << At->getColumn();
    else if (printAt(dyn_cast_or_null<DILexicalBlock>(At->getScope()), "[", O))
      O << "]";
    return true;
  }
  return false;
}

void printDILocationSource(unsigned DWLang,
    const DIMemory &Loc, llvm::raw_ostream &O) {
  auto M = const_cast<DIMemory *>(&Loc);
  auto printDbgLoc = [&Loc, &O]() {
    SmallVector<DebugLoc, 1> DbgLocs;
    Loc.getDebugLoc(DbgLocs);
    auto NumberOfLocs = count_if(DbgLocs, [](const DebugLoc &L) {
      return L.getLine() > 0;
    });
    if (NumberOfLocs == 1) {
      for (auto &L : DbgLocs)
        printAt(DbgLocs.front().get(), ":", O);
    } else if (NumberOfLocs > 1) {
      O << ":{";
      bool WasPrinted = false;
      for (auto &L: DbgLocs) {
        if (WasPrinted)
          printAt(L.get(), "|", O);
        else
          WasPrinted = printAt(L.get(), "", O);
      }
      O << "}";
    }
  };
  if (auto EM = dyn_cast<DIEstimateMemory>(M)) {
    auto TmpLoc{DIMemoryLocation::get(EM->getVariable(), EM->getExpression(),
                                      nullptr, EM->isTemplate(),
                                      EM->isAfterPointer())};
    if (!TmpLoc.isValid()) {
      O << "<";
      O << "sapfor.invalid";
      if (TmpLoc.Var)
        O << "(" << TmpLoc.Var->getName() << ")";
      printDbgLoc();
      O << ",?>";
      return;
    }
    O << "<";
    if (!unparsePrint(DWLang, TmpLoc, O))
      O << "?" << TmpLoc.Var->getName() << "?";
    printDbgLoc();
    O << ", ";
    auto Size = TmpLoc.getSize();
    if (!Size.hasValue())
      if (Size.mayBeBeforePointer())
        O << "?";
      else
        O << "+?";
    else
      O << Size.getValue();
    O << ">";
  } else if (auto UM = dyn_cast<DIUnknownMemory>(M)) {
    auto MD = UM->getMetadata();
    assert(MD && "MDNode must not be null!");
    if (UM->isExec()) {
      if (!isa<DISubprogram>(MD))
        O << "?()";
      else
        O << cast<DISubprogram>(MD)->getName() << "()";
      printDbgLoc();
    } else if (UM->isResult()) {
      if (!isa<DISubprogram>(MD))
        O << "<?()";
      else
        O << "<" << cast<DISubprogram>(MD)->getName() << "()";
      printDbgLoc();
      O << ",?>";
    } else {
      if (isa<DISubprogram>(MD)) {
        O << "<*" << cast<DISubprogram>(MD)->getName() << ",?>";
      } else if (isa<DIVariable>(MD)) {
        O << "<*" << cast<DIVariable>(MD)->getName() << ",?>";
      } else {
        SmallString<32> Address("?");
        if (MD->getNumOperands() == 1)
          if (auto Const = dyn_cast<ConstantAsMetadata>(MD->getOperand(0))) {
            auto CInt = cast<ConstantInt>(Const->getValue());
            Address.front() = '*';
            CInt->getValue().toStringUnsigned(Address);
          }
        O << "<" << Address;
        printDbgLoc();
        O << ",?>";
      }
    }
  } else {
    O << "<sapfor.invalid,?>";
  }
}

void printDIType(raw_ostream &o, const DIType *DITy) {
  bool isDerived = false;
  if (auto *Ty = dyn_cast_or_null<DIDerivedType>(DITy)) {
    DITy= Ty->getBaseType();
    isDerived = true;
  }
  if (DITy)
    o << DITy->getName();
  else
    o << "<unknown type>";
  if (isDerived)
    o << "*";
}

void printDIVariable(raw_ostream &o, DIVariable *DIVar) {
  assert(DIVar && "Variable must not be null!");
  o << DIVar->getLine() << ": ";
  printDIType(o, DIVar->getType()), o << " ";
  o << DIVar->getName();
}

Optional<VariableLocationT> buildVariable(unsigned DWLang,
                                          const DIEstimateMemory &DIEM,
                                          SmallVectorImpl<char> &Path) {
  VariableLocationT Var;
  auto *DIVar{DIEM.getVariable()};
  if (isStubVariable(*DIVar))
    return None;
  auto *DIExpr{DIEM.getExpression()};
  assert(DIVar && DIExpr && "Invalid memory location!");
  SmallVector<DebugLoc, 1> DbgLocs;
  DIEM.getDebugLoc(DbgLocs);
  DILocation *DefinitionLoc{nullptr};
  if (DbgLocs.empty()) {
    // Column may be omitted for global variables only, because there are
    // no more than one variable with the same name in a global scope.
    if (!isa<DIGlobalVariable>(DIVar))
      return None;
    DefinitionLoc = DILocation::get(DIVar->getContext(), DIVar->getLine(), 0,
                                    DIVar->getScope());
  } else if (DbgLocs.size() > 1) {
    DefinitionLoc = DbgLocs.front().get();
    for (auto &DbgLoc : DbgLocs)
      if (DbgLoc.getLine() < DefinitionLoc->getLine())
        DefinitionLoc = DbgLoc.get();
      else if (DbgLoc.getLine() == DefinitionLoc->getLine() &&
               DbgLoc.getCol() < DefinitionLoc->getColumn())
        DefinitionLoc = DbgLoc.get();
  } else {
    DefinitionLoc = DbgLocs.front().get();
  }
  Var.get<Line>() = DefinitionLoc->getLine();
  Var.get<Column>() = DefinitionLoc->getColumn();
  SmallString<32> LocToString;
  auto TmpLoc{DIMemoryLocation::get(
      const_cast<DIVariable *>(DIEM.getVariable()),
      const_cast<DIExpression *>(DIEM.getExpression()), DefinitionLoc,
      DIEM.isTemplate(), DIEM.isAfterPointer())};
  if (!unparseToString(DWLang, TmpLoc, LocToString))
    return None;
  std::replace(LocToString.begin(), LocToString.end(), '*', '^');
  Var.get<Identifier>() = std::string(LocToString);
  sys::fs::UniqueID FileID;
  if (sys::fs::getUniqueID(getAbsolutePath(*DIVar->getFile(), Path), FileID))
    return None;
  Var.get<File>() = FileID;
  return Var;
}

Optional<VariableLocationT> buildVariable(unsigned DWLang,
                                          const DIEstimateMemory &DIEM) {
  SmallString<128> Path;
  return buildVariable(DWLang, DIEM, Path);
}
}
