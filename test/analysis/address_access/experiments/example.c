//
// Created by Vladislav Volodkin on 10/18/20.
//
#include <stdlib.h>

int** glob;

void fun1(int *a) { // %a detected as captured 
  int **ptr;
  ptr = (int**) malloc(sizeof(int*));
  glob = ptr;  // without this instr analysis proves %a it uncaptured

  int *c = &(a[3]);
  *ptr = c;
}

int main() {
  return 0;
}

/*
Whole function: 
  %a.addr = alloca i32*, align 8
  %ptr = alloca i32**, align 8
  %c = alloca i32*, align 8
  store i32* %a, i32** %a.addr, align 8, !tbaa !23
  call void @llvm.dbg.declare(metadata i32** %a.addr, metadata !20, metadata !DIExpression()), !dbg !27
  %0 = bitcast i32*** %ptr to i8*, !dbg !28
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %0) #4, !dbg !28
  call void @llvm.dbg.declare(metadata i32*** %ptr, metadata !21, metadata !DIExpression()), !dbg !29
  %call = call i8* @malloc(i64 8) #5, !dbg !30
  %1 = bitcast i8* %call to i32**, !dbg !31
  store i32** %1, i32*** %ptr, align 8, !dbg !32, !tbaa !23
  %2 = load i32**, i32*** %ptr, align 8, !dbg !33, !tbaa !23
  store i32** %2, i32*** @glob, align 8, !dbg !34, !tbaa !23
  %3 = bitcast i32** %c to i8*, !dbg !35
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %3) #4, !dbg !35
  call void @llvm.dbg.declare(metadata i32** %c, metadata !22, metadata !DIExpression()), !dbg !36
  %4 = load i32*, i32** %a.addr, align 8, !dbg !37, !tbaa !23
  %arrayidx = getelementptr inbounds i32, i32* %4, i64 3, !dbg !37
  store i32* %arrayidx, i32** %c, align 8, !dbg !36, !tbaa !23
  %5 = load i32*, i32** %c, align 8, !dbg !38, !tbaa !23
  %6 = load i32**, i32*** %ptr, align 8, !dbg !39, !tbaa !23
  store i32* %5, i32** %6, align 8, !dbg !40, !tbaa !23
  %7 = bitcast i32** %c to i8*, !dbg !41
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %7) #4, !dbg !41
  %8 = bitcast i32*** %ptr to i8*, !dbg !41
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %8) #4, !dbg !41
  ret void, !dbg !41
*/
