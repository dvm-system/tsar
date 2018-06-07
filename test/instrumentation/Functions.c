#include <stdio.h>
#include <stdlib.h>

void sapforRegVar(void* DIVar, void* Addr) {
  printf("called sapforRegVar\n");	
  printf("DiVar = %s\n\n", DIVar);
}

void sapforRegArr(void* DIVar, size_t ArrSize, void* Addr) {
  printf("called sapforRegArr\n");	
  printf("DiVar = %s\nArrSize = %zu\n\n", DIVar, ArrSize);
}

void sapforReadVar(void* DILoc, void* Addr, void* DIVar) {
  printf("called sapforReadVar\n");	
  printf("DiVar = %s\n", DIVar);
  printf("DiLoc = %s\n\n", DILoc);
}

void sapforReadArr(void* DILoc, void* Addr, void* DIVar, void* ArrBase) {
  printf("called sapforReadArr\n");	
  printf("DiVar = %s\n", DIVar);
  printf("DiLoc = %s\n\n", DILoc);
}

void sapforWriteVarEnd(void* DILoc, void* Addr, void* DIVar) {
  printf("called sapforWriteVarEnd\n");	
  printf("DiVar = %s\n", DIVar);
  printf("DiLoc = %s\n\n", DILoc);
}

void sapforWriteArrEnd(void* DILoc, void* Addr, void* DIVar, void* ArrBase) {
  printf("called sapforWriteArrEnd\n");	
  printf("DiVar = %s\n", DIVar);
  printf("DiLoc = %s\n\n", DILoc);
}

void sapforFuncBegin(void* DIFunc) {
  printf("called sapforFuncBegin\n");
  printf("DiFunc = %s\n\n", DIFunc);
}  

void sapforFuncEnd(void* DIFunc) {
  printf("called sapforFuncEnd\n");
  printf("DiFunc = %s\n\n", DIFunc);
}  

void sapforRegDummyVar(void* DIFunc, void* Addr, size_t Position) {
  printf("called sapforRegDummyVar\n");
  printf("DiFunc = %s\n Position = %zu\n\n", DIFunc, Position);
}

void sapforRegDummyArr(void* DIFunc, size_t ArrSize,void* Addr,size_t Position){
  printf("called sapforRegDummyArr\n");
  printf("DiFunc = %s\nPosition = %zu\nArrSize = %zu\n\n", DIFunc, Position,
    ArrSize);
}

void sapforFuncCallBegin(void* DICall) {
  printf("called sapforFuncCallBegin\n");
  printf("DiCall = %s\n\n", DICall);
}  

void sapforFuncCallEnd(void* DICall) {
  printf("called sapforFuncCallEnd\n");
  printf("DiCall = %s\n\n", DICall);
}  

void sapforSLBegin(void* DILoop, long First, long Last, long Step) {
  printf("called sapforSLBegin\n");	
  printf("DiLoop = %s\nFirst = %ld Last = %ld, Step = %ld\n\n", DILoop, First,
    Last, Step);
}

void sapforSLEnd(void* DILoop) {
  printf("called sapforSLEnd\n");	
  printf("DiLoop = %s\n\n", DILoop);
}

void sapforSLIter(void* DILoop, void* Addr) {
  printf("called sapforSLIter\n");	
  printf("DILoop = %s\n\n", DILoop);
}

void sapforInitDI(void** DI, char* DIString) {
  printf("called sapforInitDI\n");
  printf("DIString = %s\n\n", DIString);
  *DI = DIString;
}

void sapforAllocatePool(void*** PoolPtr, size_t Size) {
  printf("called sapforAllocatePool\n");
  printf("Size = %zu\n\n", Size);
  *PoolPtr = (void**) malloc(Size * sizeof(void**));
}

void sapforDeclTypes(size_t Num, size_t* Ids, size_t* Sizes) {
  size_t I;
  printf("called sapforDeclTypes\n");
  printf("Num = %zu\n\n", Num);
  for(I = 0; I < Num; ++I) {
    printf("it = %zu ids = %zu size = %zu\n", I, Ids[I], Sizes[I]);
  }
  printf("\n");
}
