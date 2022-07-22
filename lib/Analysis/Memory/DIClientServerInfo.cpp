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

void static initialize(AnalysisSocket &Socket, llvm::Function &F,
                       DIAliasTree *ClientDIAT,
                       DIDependencInfo *ClientDIDepInfo,
                       DIClientServerInfo &This) {
    if (auto R = Socket.getAnalysis<AnalysisClientServerMatcherWrapper,
                                     ClonedDIMemoryMatcherWrapper>()) {
      auto *Matcher = R->value<AnalysisClientServerMatcherWrapper *>();
      This.getMetadata = [Matcher](Metadata *MD) {
        assert(MD && "Metadata must not be null!");
        auto ServerMD = (*Matcher)->getMappedMD(MD);
        return ServerMD ? *ServerMD : nullptr;
      };
      This.getObjectID = [Matcher](ObjectID ID) {
        assert(ID && "Object ID must not be null!");
        auto ServerID = (*Matcher)->getMappedMD(ID);
        return ServerID ? cast<MDNode>(*ServerID) : nullptr;
      };
      This.getValue = [Matcher](Value *V) {
        assert(V && "Value must not be null!");
        return (**Matcher)[V];
      };
      auto ServerV = This.getValue(&F);
      if (ServerV) {
        auto ServerF = cast<Function>(ServerV);
        auto *MemoryMatcher =
            (**R->value<ClonedDIMemoryMatcherWrapper *>())[*ServerF];
        assert(MemoryMatcher && "Memory matcher must exist!");
        This.getMemory = [MemoryMatcher](DIMemory *M) {
          assert(M && "Memory must not be null!");
          auto Itr = MemoryMatcher->find<Origin>(M);
          return Itr != MemoryMatcher->end() ? Itr->get<Clone>() : nullptr;
        };
        This.getClientMemory = [MemoryMatcher](DIMemory *M) {
          assert(M && "Memory must not be null!");
          auto Itr = MemoryMatcher->find<Clone>(M);
          return Itr != MemoryMatcher->end() ? Itr->get<Origin>() : nullptr;
        };
        if (auto R = Socket.getAnalysis<DIEstimateMemoryPass,
                                         DIDependencyAnalysisPass>(F)) {
          This.DIAT = &R->value<DIEstimateMemoryPass *>()->getAliasTree();
          This.DIDepInfo =
              &R->value<DIDependencyAnalysisPass *>()->getDependencies();
        }
      }
    }
}

DIClientServerInfo::DIClientServerInfo(llvm::Pass &P, llvm::Function &F,
                                       DIAliasTree *ClientDIAT,
                                       DIDependencInfo *ClientDIDepInfo) {
  if (auto *SI{P.getAnalysisIfAvailable<AnalysisSocketImmutableWrapper>()})
    if (auto *Socket{(*SI)->getActiveSocket()})
      initialize(*Socket, F, ClientDIAT, ClientDIDepInfo, *this);
  if (!DIAT || !DIDepInfo) {
    LLVM_DEBUG(dbgs() << "[SERVER INFO]: analysis server is not available\n");
    DIAT = ClientDIAT;
    DIDepInfo = ClientDIDepInfo;
    if (!DIAT)
      if (auto *DIATP = P.getAnalysisIfAvailable<DIEstimateMemoryPass>())
        if (DIATP->isConstructed())
          DIAT = &DIATP->getAliasTree();
    if (!DIDepInfo)
      if (auto *DIDepP = P.getAnalysisIfAvailable<DIDependencyAnalysisPass>())
        DIDepInfo = &DIDepP->getDependencies();
    LLVM_DEBUG(
      if (isValid())
        dbgs() << "[SERVER INFO]: use dependence analysis from client\n");
  }
}

DIClientServerInfo::DIClientServerInfo(AnalysisSocket *Socket,
                                       llvm::Function &F,
                                       DIAliasTree *ClientDIAT,
                                       DIDependencInfo *ClientDIDepInfo) {
  if (Socket)
    initialize(*Socket, F, ClientDIAT, ClientDIDepInfo, *this);
  if (!DIAT || !DIDepInfo) {
    LLVM_DEBUG(dbgs() << "[SERVER INFO]: analysis server is not available\n");
    DIAT = ClientDIAT;
    DIDepInfo = ClientDIDepInfo;
    LLVM_DEBUG(if (isValid()) dbgs()
               << "[SERVER INFO]: use dependence analysis from client\n");
  }
}

DIMemoryClientServerInfo::DIMemoryClientServerInfo(DIAliasTree &ClientDIAT,
    llvm::Pass &P, llvm::Function &F)
  : DIClientServerInfo(P, F), ClientDIAT(&ClientDIAT) {}

DIMemoryClientServerInfo::DIMemoryClientServerInfo(
    DIAliasTree &ClientDIAT, AnalysisSocket *Socket, llvm::Function &F,
    DIDependencInfo *ClientDIDepInfo)
    : DIClientServerInfo(Socket, F, &ClientDIAT, ClientDIDepInfo),
      ClientDIAT(&ClientDIAT) {}

bcl::tagged_pair<
  bcl::tagged<DIMemory *, Origin>, bcl::tagged<DIMemory *, Clone>>
DIMemoryClientServerInfo::findFromClient(const EstimateMemory &EM,
    const DataLayout &DL, DominatorTree &DT) const {
  assert(isValidMemory() && "Results is not available!");
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
    DIUnknownMemory::Flags F) const {
  assert(isValidMemory() && "Results is not available!");
  auto RawDIM = getRawDIMemoryIfExists(V, V.getContext(), DT, F);
  if (!RawDIM)
    return std::make_pair(nullptr, nullptr);
  auto ClientDIMItr = ClientDIAT->find(*RawDIM);
  if (ClientDIMItr == ClientDIAT->memory_end())
    return std::make_pair(nullptr, nullptr);
  return std::make_pair(&*ClientDIMItr, getMemory(&*ClientDIMItr));
}

DIDependenceSet *DIMemoryClientServerInfo::findFromClient(const Loop &L) const {
  assert(isValid() && "Results is not available!");
  if (auto ClientID = L.getLoopID())
    if (auto LoopID = getObjectID(ClientID)) {
      auto DIDepItr = DIDepInfo->find(LoopID);
      return DIDepItr != DIDepInfo->end() ?
        &DIDepItr->get<DIDependenceSet>() : nullptr;
    }
  return nullptr;
}
