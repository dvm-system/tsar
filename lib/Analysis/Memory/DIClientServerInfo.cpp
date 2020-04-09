//=== DIClientServerInfo.h - Client Server Analysis Matcher -----*- C++ -*-===//
//
//                       Traits Static Analyzer (SAPFOR)
//
// Copyright 2020 DVM System Group
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
// This file defines is a helpful class to obtain analysis results from server.
//
//===----------------------------------------------------------------------===//

#include "tsar/Analysis/Memory/DIClientServerInfo.h"
#include "tsar/Analysis/Memory/EstimateMemory.h"
#include "tsar/Analysis/AnalysisServer.h"
#include "tsar/Analysis/Memory/ClonedDIMemoryMatcher.h"

#define DEBUG_TYPE "client-server"

using namespace tsar;
using namespace llvm;

DIClientServerInfo::DIClientServerInfo(llvm::Pass &P, llvm::Function &F) {
  if (auto *SI = P.getAnalysisIfAvailable<AnalysisSocketImmutableWrapper>()) {
    if (auto *Socket = (*SI)->getActiveSocket()) {
      if (auto R = Socket->getAnalysis<AnalysisClientServerMatcherWrapper,
                                       ClonedDIMemoryMatcherWrapper>()) {
        auto *Matcher = R->value<AnalysisClientServerMatcherWrapper *>();
        getMetadata = [Matcher](Metadata *MD) {
          assert(MD && "Metadata must not be null!");
          auto ServerMD = (*Matcher)->getMappedMD(MD);
          return ServerMD ? *ServerMD : nullptr;
        };
        getObjectID = [Matcher](ObjectID ID) {
          assert(ID && "Object ID must not be null!");
          auto ServerID = (*Matcher)->getMappedMD(ID);
          return ServerID ? cast<MDNode>(*ServerID) : nullptr;
        };
        getValue = [Matcher](Value *V) {
          assert(V && "Value must not be null!");
          return (**Matcher)[V];
        };
        auto ServerF = cast<Function>(getValue(&F));
        assert(ServerF && "Mapped function must exist on server!");
        auto *MemoryMatcher =
            (**R->value<ClonedDIMemoryMatcherWrapper *>())[*ServerF];
        assert(MemoryMatcher && "Memory matcher must exist!");
        getMemory = [MemoryMatcher](DIMemory *M) {
          assert(M && "Memory must not be null!");
          auto Itr = MemoryMatcher->find<Origin>(M);
          return Itr != MemoryMatcher->end() ? Itr->get<Clone>() : nullptr;
        };
        if (auto R = Socket->getAnalysis<
              DIEstimateMemoryPass, DIDependencyAnalysisPass>(F)) {
          DIAT = &R->value<DIEstimateMemoryPass *>()->getAliasTree();
          DIDepInfo =
              &R->value<DIDependencyAnalysisPass *>()->getDependencies();
        }
      }
    }
  }
  if (!DIAT || !DIDepInfo) {
    LLVM_DEBUG(dbgs() << "[SERVER INFO]: analysis server is not available\n");
    if (auto *DIATP = P.getAnalysisIfAvailable<DIEstimateMemoryPass>())
      DIAT = &DIATP->getAliasTree();
    else
      return;
    if (auto *DIDepP = P.getAnalysisIfAvailable<DIDependencyAnalysisPass>())
      DIDepInfo = &DIDepP->getDependencies();
    else
      return;
    LLVM_DEBUG(
        dbgs() << "[SERVER INFO]: use dependence analysis from client\n");
  }
}

DIMemoryClientServerInfo::DIMemoryClientServerInfo(
    llvm::Pass &P, llvm::Function &F) : DIClientServerInfo(P, F) {
  if (auto *DIATP = P.getAnalysisIfAvailable<DIEstimateMemoryPass>())
    ClientDIAT = &DIATP->getAliasTree();
  else
    return;
}

bcl::tagged_pair<
  bcl::tagged<DIMemory *, Origin>, bcl::tagged<DIMemory *, Clone>>
DIMemoryClientServerInfo::findFromClient(EstimateMemory &EM,
    const DataLayout &DL, DominatorTree &DT) {
  assert(isValid() && "Results is not available!");
  auto RawDIM = getRawDIMemoryIfExists(EM, EM.front()->getContext(), DL, DT);
  if (!RawDIM)
    return std::make_pair(nullptr, nullptr);
  auto ClientDIMItr = ClientDIAT->find(*RawDIM);
  if (ClientDIMItr == ClientDIAT->memory_end())
    return std::make_pair(nullptr, nullptr);
  return std::make_pair(&*ClientDIMItr, getMemory(&*ClientDIMItr));
}

bcl::tagged_pair<
  bcl::tagged<DIMemory *, Origin>, bcl::tagged<DIMemory *, Clone>>
DIMemoryClientServerInfo::findFromClient(Value &V, DominatorTree &DT,
    DIUnknownMemory::Flags F) {
  assert(isValid() && "Results is not available!");
  auto RawDIM = getRawDIMemoryIfExists(V, V.getContext(), DT, F);
  if (!RawDIM)
    return std::make_pair(nullptr, nullptr);
  auto ClientDIMItr = ClientDIAT->find(*RawDIM);
  if (ClientDIMItr == ClientDIAT->memory_end())
    return std::make_pair(nullptr, nullptr);
  return std::make_pair(&*ClientDIMItr, getMemory(&*ClientDIMItr));
}

DIDependenceSet *DIMemoryClientServerInfo::findFromClient(const Loop &L) {
  assert(isValid() && "Results is not available!");
  if (auto ClientID = L.getLoopID())
    if (auto LoopID = getObjectID(ClientID)) {
      auto DIDepItr = DIDepInfo->find(LoopID);
      return DIDepItr != DIDepInfo->end() ?
        &DIDepItr->get<DIDependenceSet>() : nullptr;
    }
}

