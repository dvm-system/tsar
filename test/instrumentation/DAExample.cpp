//===--- DAExample.h ----- Dynamic Analyzer Example ------------*- C++ -*-===//
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
// This file defines functions of dynamic analyzer. Instrumentation pass inserts
// calls of these functions into LLVM IR.
//
// Usage:
// (1) tsar -instr-llvm Example.c
// (2) clang -std=c++11 Example.ll DAExample.cpp
// (3) ./a.out or Example.exe (in case of Windows)
//
// Note: use clang-cl on Windows OS.
//
//===----------------------------------------------------------------------===//

#include <cstdint>
#include <regex>
#include <stdio.h>
#include <stdlib.h>
#include <string>

using namespace std;

extern "C" {
//===------ Initialization of metadata and registration of types ----------===//
void sapforInitDI(void **DI, char *DIString) {
  printf("called sapforInitDI\n");
  printf("DIString = %s\n\n", DIString);
  *DI = DIString;
}

void sapforAllocatePool(void ***PoolPtr, uint64_t Size) {
  printf("called sapforAllocatePool\n");
  printf("Size = %zu\n\n", Size);
  *PoolPtr = (void**) malloc(Size * sizeof(void**));
}

void sapforDeclTypes(uint64_t Num, uint64_t *Ids, uint64_t* Sizes) {
  printf("called sapforDeclTypes\n");
  printf("Num = %ju\n\n", Num);
  for(uint64_t I = 0; I < Num; ++I) {
    printf("Idx = %ju TypeId = %ju TypeSize = %ju\n", I, Ids[I], Sizes[I]);
  }
  printf("\n");
}

//===------------------ Registration of memory accesses -------------------===//
void sapforRegVar(void *DIVar, void *Addr) {
  printf("called sapforRegVar\n");
  printf("DIVar = %s\n\n", DIVar);
}

void sapforRegArr(void *DIVar, uint64_t ArrSize, void* Addr) {
  printf("called sapforRegArr\n");
  printf("DIVar = %s\nArrSize = %ju\n\n", DIVar, ArrSize);
}

void sapforReadVar(void *DILoc, void* Addr, void* DIVar) {
  printf("called sapforReadVar\n");	
  printf("DIVar = %s\nDILoc = %s\n\n", DIVar, DILoc);
}

void sapforReadArr(void *DILoc, void *Addr, void *DIVar, void* ArrBase) {
  printf("called sapforReadArr\n");	
  printf("DIVar = %s\nDILoc = %s\n\n", DIVar, DILoc);
}

void sapforWriteVarEnd(void *DILoc, void *Addr, void *DIVar) {
  printf("called sapforWriteVarEnd\n");	
  printf("DIVar = %s\nDILoc = %s\n\n", DIVar, DILoc);
}

void sapforWriteArrEnd(void *DILoc, void *Addr, void *DIVar, void *ArrBase) {
  printf("called sapforWriteArrEnd\n");	
  printf("DIVar = %s\nDILoc = %s\n\n", DIVar, DILoc);
}

//===--------------------- Registration of a function ---------------------===//
void sapforFuncBegin(void *DIFunc) {
  printf("called sapforFuncBegin\n");
  printf("DIFunc = %s\n\n", DIFunc);
}  

void sapforFuncEnd(void *DIFunc) {
  printf("called sapforFuncEnd\n");
  printf("DIFunc = %s\n\n", DIFunc);
}  

void sapforRegDummyVar(void *DIVar, void *Addr, void *DIFunc,
    uint64_t Position) {
  printf("called sapforRegDummyVar\n");
  printf("DIVar = %s\nDIFunc = %s\nPosition = %zu\n\n",
    DIVar, DIFunc, Position);
}

void sapforRegDummyArr(void *DIVar, uint64_t ArrSize, void* Addr,
    void *DIFunc, uint64_t Position) {
  printf("called sapforRegDummyArr\n");
  printf("DIVar = %s\nArrSize = %ju\nDIFunc = %s\nPosition = %zu\n\n",
    DIVar, ArrSize, DIFunc, Position);
}

void sapforFuncCallBegin(void *DILoc, void *DIFunc) {
  printf("called sapforFuncCallBegin\n");
  printf("DILoc = %s\n\nDIFunc = %s\n\n", DILoc, DIFunc);
}  

void sapforFuncCallEnd(void *DIFunc) {
  printf("called sapforFuncCallEnd\n");
  printf("DIFunc = %s\n\n", DIFunc);
}  

//===---------------------- Registration of a loop ------------------------===//
void sapforSLBegin(void *DILoop, uint64_t Start, uint64_t End, uint64_t Step) {
  printf("called sapforSLBegin\n");
  printf("DiLoop = %s\n", DILoop);
  std::cmatch cm;
  if (std::regex_search((char *)DILoop, cm, std::regex(R"(bounds=(\d))"))) {
    unsigned Flag = stoul(cm.str(1).c_str());
    bool IsSigned = !((Flag >> 3) & 1u);
    bool HasStart = (Flag >> 0) & 1u;
    bool HasEnd = (Flag >> 1) & 1u;
    bool HasStep = (Flag >> 2) & 1u;
    if (HasStart)
      if (IsSigned)
        printf("Start=%jd\n", Start);
      else
        printf("Start=%jo\n", Start);
    if (HasEnd)
      if (IsSigned)
        printf("End=%jd\n", End);
      else
        printf("End=%jo\n", End);
    if (HasStep)
      if (IsSigned)
        printf("Step=%jd\n", Step);
      else
        printf("Step=%jo\n", Step);
  }
}

void sapforSLEnd(void *DILoop) {
  printf("called sapforSLEnd\n");	
  printf("DILoop = %s\n\n", DILoop);
}

void sapforSLIter(void *DILoop, uint64_t Iter) {
  printf("called sapforSLIter\n");	
  printf("DILoop = %s\n\n", DILoop);
  printf("Iteration = %lld\n", Iter);
}
}
