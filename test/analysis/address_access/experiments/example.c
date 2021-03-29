//
// Created by Vladislav Volodkin on 10/18/20.
//
#include <stdlib.h>

int** glob;

void fun1(int *a) { // captured 
  int **ptr;
  ptr = (int**) malloc(sizeof(int*));
  glob = ptr;  // without this instr it's not captured (analysis works)

  int *c = &(a[3]);
  *ptr = c;
}

int main() {
  return 0;
}


/*
@glob, %ptr, %a.addr, %2, %8, %call, %3, %a, %6, %arridx

Whole function: 
  %a.addr = alloca i32*, align 8
  %ptr = alloca i32**, align 8
  %c = alloca i32*, align 8
  store i32* %a, i32** %a.addr, align 8, !tbaa !23
  call void @llvm.dbg.declare(metadata i32** %a.addr, metadata !20, metadata !DIExpression()), !dbg !27
  %0 = bitcast i32*** %ptr to i8*, !dbg !28
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %0) #5, !dbg !28
  call void @llvm.dbg.declare(metadata i32*** %ptr, metadata !21, metadata !DIExpression()), !dbg !29
  br label %1, !dbg !30
  %call = call i8* @malloc(i64 8) #6, !dbg !30
  br label %2, !dbg !31
  %3 = bitcast i8* %call to i32**, !dbg !31
  store i32** %3, i32*** %ptr, align 8, !dbg !32, !tbaa !23
  %4 = load i32**, i32*** %ptr, align 8, !dbg !33, !tbaa !23
  store i32** %4, i32*** @glob, align 8, !dbg !34, !tbaa !23
  %5 = bitcast i32** %c to i8*, !dbg !35
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %5) #5, !dbg !35
  call void @llvm.dbg.declare(metadata i32** %c, metadata !22, metadata !DIExpression()), !dbg !36
  %6 = load i32*, i32** %a.addr, align 8, !dbg !37, !tbaa !23
  %arrayidx = getelementptr inbounds i32, i32* %6, i64 3, !dbg !37
  store i32* %arrayidx, i32** %c, align 8, !dbg !36, !tbaa !23
  %7 = load i32*, i32** %c, align 8, !dbg !38, !tbaa !23
  %8 = load i32**, i32*** %ptr, align 8, !dbg !39, !tbaa !23
  store i32* %7, i32** %8, align 8, !dbg !40, !tbaa !23
  %9 = bitcast i32** %c to i8*, !dbg !41
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %9) #5, !dbg !41
  %10 = bitcast i32*** %ptr to i8*, !dbg !41
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %10) #5, !dbg !41
  ret void, !dbg !41
*/

/*
Analyzing function: 
a,
Met store:   store i32* %a, i32** %a.addr, align 8, !tbaa !25
Met alloca memory destination:   %a.addr = alloca i32*, align 8
a.addr,
Met load:   %6 = load i32*, i32** %a.addr, align 8, !dbg !41, !tbaa !25
Met store:   store i32* %a, i32** %a.addr, align 8, !tbaa !25
,
Met gep:   %arrayidx = getelementptr inbounds i32, i32* %6, i64 3, !dbg !41
arrayidx,
Met store:   store i32* %arrayidx, i32** %c, align 8, !dbg !40, !tbaa !25
Met alloca memory destination:   %c = alloca i32*, align 8
c,
Met bitcast:   %9 = bitcast i32** %c to i8*, !dbg !45
Met load:   %7 = load i32*, i32** %c, align 8, !dbg !42, !tbaa !25
Met store:   store i32* %arrayidx, i32** %c, align 8, !dbg !40, !tbaa !25
Met bitcast:   %5 = bitcast i32** %c to i8*, !dbg !39
Arg may be captured: a
b,
Met store:   store i32* %b, i32** %b.addr, align 8, !tbaa !25
Met alloca memory destination:   %b.addr = alloca i32*, align 8
b.addr,
Met store:   store i32* %b, i32** %b.addr, align 8, !tbaa !25
Proved to be not captured: b
ptra,
Met store:   store i32** %ptra, i32*** %ptra.addr, align 8, !tbaa !25
Met alloca memory destination:   %ptra.addr = alloca i32**, align 8
ptra.addr,
Met store:   store i32** %ptra, i32*** %ptra.addr, align 8, !tbaa !25
Proved to be not captured: ptra
*/