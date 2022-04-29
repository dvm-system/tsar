//===- APCContext.h - Class for managing parallelization state  -*- C++ -*-===//
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
// This file implements a container of a state of automated parallelization
// process in SAPFOR.
//
//===----------------------------------------------------------------------===//

#include "APCContextImpl.h"
#include "tsar/APC/APCContext.h"
#include "tsar/APC/Passes.h"
#include <llvm/IR/Module.h>
#include <llvm/Pass.h>
#include <map>

using namespace tsar;
using namespace llvm;

//===--------------- Necessary External Objects----------------------------===//
//
// The following items is necessary for linking but do not used at this moment.

/// Global map from a file name to a list of functions it contains.
std::map<std::string, std::vector<FuncInfo*>> allFuncInfo;

/// Global map from a file name to a parent of some loop tree.
///
/// This map is necessary for APC and contains a list of outer loops for each
/// file in a currently processed project.
std::map<std::string, std::vector<LoopGraph*>> loopGraph;

/// Global map from a file name to a list of diagnostic messages.
std::map<std::string, std::vector<Messages>> SPF_messages;

void sendMessage_1lvl(const std::wstring &toSend) {}
void sendMessage_2lvl(const std::wstring &toSend) {}

int langOfMessages = 1; // 0 - ENG, 1 - RUS
int mpiProgram = 0;
int keepFiles = 0;

/// Try to transform nest of loops to perfect nest.
bool createNestedLoops(LoopGraph *current,
    const std::map<LoopGraph *, void *> &depInfoForLoopGraph,
    const std::map<std::string, FuncInfo *> &mapFuncInfo,
    std::vector<Messages> &messages) {
  return false;
}
bool analyzeLoopBody(
    LoopGraph *loopV,
    std::map<DIST::Array *,
             std::vector<std::pair<bool, std::pair<std::string, int>>>>
        &leftValues,
    std::map<DIST::Array *,
             std::vector<
                 std::pair<bool, std::map<std::string, std::pair<int, int>>>>>
        &rightValues,
    std::string &base,
    const std::map<DIST::Array *, std::vector<bool>> &dimsNotMatch,
    const std::map<std::string, FuncInfo *> &mapFuncInfo) {
  return true;
}

void createNeededException() { }

//===----------------------------------------------------------------------===//

APCContext::APCContext() : mImpl(new APCContextImpl) {}
APCContext::~APCContext() { delete mImpl; }

void APCContext::initialize() {
  assert(!mIsInitialized && "Context has been already initialized!");
  mImpl->ParallelRegions.push_back(
    std::make_unique<ParallelRegion>(mImpl->ParallelRegions.size(), "DEFAULT"));
#ifndef NDEBUG
  mIsInitialized = true;
#endif
}

ParallelRegion & APCContext::getDefaultRegion() {
  assert(mIsInitialized && "Context must be initialized!");
  return *mImpl->ParallelRegions.front();
}

void APCContext::addExpression(apc::Expression *E) {
  mImpl->Expressions.emplace_back(E);
}

void APCContext::addSymbol(apc::Symbol *S) {
  mImpl->Symbols.emplace_back(S);
}

void APCContext::addStatement(apc::Statement *S) {
  mImpl->Statements.emplace_back(S);
}

bool APCContext::addLoop(ObjectID ID, apc::LoopGraph *L, bool ManageMemory) {
  if (!mImpl->Loops.try_emplace(ID, L).second)
    return false;
  if (ManageMemory)
    mImpl->OuterLoops.emplace_back(L);
  return true;
}

apc::LoopGraph * APCContext::findLoop(ObjectID ID) {
  auto I = mImpl->Loops.find(ID);
  return I != mImpl->Loops.end() ? I->second : nullptr;
}

bool APCContext::addArray(ObjectID ID, apc::Array *A) {
  return mImpl->Arrays.try_emplace(ID, A).second;
}

bool APCContext::addArray(dvmh::Template *ID, apc::Array *A) {
  return mImpl->Templates.try_emplace(ID, A).second;
}

apc::Array* APCContext::findArray(ObjectID ID) {
  auto I = mImpl->Arrays.find(ID);
  return I != mImpl->Arrays.end() ? I->second.get() : nullptr;
}

apc::Array* APCContext::findArray(dvmh::Template *ID) {
  auto I = mImpl->Templates.find(ID);
  return I != mImpl->Templates.end() ? I->second : nullptr;
}

std::size_t APCContext::getNumberOfArrays() const {
  return mImpl->Arrays.size();
}

bool APCContext::addFunction(llvm::Function &F, apc::FuncInfo *FI) {
  return mImpl->Functions.try_emplace(&F, FI).second;
}

apc::FuncInfo * APCContext::findFunction(const llvm::Function &F) {
  auto I = mImpl->Functions.find(&F);
  return I != mImpl->Functions.end() ? I->second.get() : nullptr;
}

std::string APCContext::getUniqueName(const DIVariable &DIVar,
                                      const Function &F) {
  auto DeclLoc{std::pair(DIVar.getLine(), 0)};
  unsigned ScopeLine{0}, ScopeColumn{0};
  auto *DIS{DIVar.getScope()};
  if (auto *DISub{dyn_cast<DISubprogram>(DIS)}) {
    ScopeLine = DISub->getLine();
  } else if (auto *DILex{dyn_cast<DILexicalBlock>(DIS)}) {
    ScopeLine = DILex->getLine();
    ScopeColumn = DILex->getColumn();
  }
  auto Filename{(DIVar.getFilename().empty()
                     ? StringRef(F.getParent()->getSourceFileName())
                     : DIVar.getFilename())};
  // Unique name is
  // '<file>:line[scopeLine:scopeColumn]:@<function>%<variable>.<member>'.
  return (Filename + ":" + Twine(DIVar.getLine()) + "[" + Twine(ScopeLine) +
          ":" + Twine(ScopeColumn) + "]" + "@" +
          (isa<DILocalVariable>(DIVar) ? F.getName() : "") + "%" +
          DIVar.getName())
      .str();
}

namespace {
/// Storage for a current state of an automated parallelization process.
class APCContextStorage : public ImmutablePass, private bcl::Uncopyable {
public:
  static char ID;
  APCContextStorage() : ImmutablePass(ID) {
    initializeAPCContextStoragePass(*PassRegistry::getPassRegistry());
  }

  void initializePass() override {
    mContext.initialize();
    getAnalysis<APCContextWrapper>().set(mContext);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<APCContextWrapper>();
  }

  APCContext & getContext() noexcept { return mContext; }
  const APCContext & getContext() const noexcept { return mContext; }
private:
  APCContext mContext;
};
}

char APCContextStorage::ID = 0;
INITIALIZE_PASS_BEGIN(APCContextStorage, "apc-context-is",
  "APC Context (Immutable Storage)", true, true)
  INITIALIZE_PASS_DEPENDENCY(APCContextWrapper)
INITIALIZE_PASS_END(APCContextStorage, "apc-context-is",
  "APC Context (Immutable Storage)", true, true)

template<> char APCContextWrapper::ID = 0;
INITIALIZE_PASS(APCContextWrapper, "apc-context-iw",
  "APC Context (Immutable Wrapper)", true, true)

ImmutablePass * llvm::createAPCContextStorage() {
  return new APCContextStorage();
}
