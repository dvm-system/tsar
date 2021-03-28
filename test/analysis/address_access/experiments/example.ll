; ModuleID = '/Users/vladislav.volodkin/CLionProjects/sapfor/analyzers/tsar/test/analysis/address_access/experiments/example.c'
source_filename = "/Users/vladislav.volodkin/CLionProjects/sapfor/analyzers/tsar/test/analysis/address_access/experiments/example.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.15.0"

@glob = global i32** null, align 8, !dbg !0

; Function Attrs: nounwind ssp uwtable
define void @fun1(i32* %a) #0 !dbg !16 {
entry:
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
}

; Function Attrs: nounwind readnone speculatable willreturn
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: argmemonly nounwind willreturn
declare void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture) #2

; Function Attrs: allocsize(0)
declare i8* @malloc(i64) #3

; Function Attrs: argmemonly nounwind willreturn
declare void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture) #2

; Function Attrs: nounwind ssp uwtable
define i32 @main() #0 !dbg !42 {
entry:
  %retval = alloca i32, align 4
  store i32 0, i32* %retval, align 4
  ret i32 0, !dbg !45
}

attributes #0 = { nounwind ssp uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "frame-pointer"="all" "less-precise-fpmad"="false" "min-legal-vector-width"="0" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind readnone speculatable willreturn }
attributes #2 = { argmemonly nounwind willreturn }
attributes #3 = { allocsize(0) "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "frame-pointer"="all" "less-precise-fpmad"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #4 = { nounwind }
attributes #5 = { allocsize(0) }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!11, !12, !13, !14}
!llvm.ident = !{!15}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "glob", scope: !2, file: !10, line: 6, type: !6, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C99, file: !3, producer: "clang version 11.1.0 (git@github.com:llvm/llvm-project.git 1fdec59bffc11ae37eb51a1b9869f0696bfd5312)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, enums: !4, retainedTypes: !5, globals: !9, nameTableKind: None, sysroot: "/")
!3 = !DIFile(filename: "/Users/vladislav.volodkin/CLionProjects/sapfor/analyzers/tsar/test/analysis/address_access/experiments/example.c", directory: "/Users/vladislav.volodkin/CLionProjects/sapfor/cmake-build-debug/analyzers/tsar/tools/tsar")
!4 = !{}
!5 = !{!6}
!6 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!7 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !8, size: 64)
!8 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!9 = !{!0}
!10 = !DIFile(filename: "analyzers/tsar/test/analysis/address_access/experiments/example.c", directory: "/Users/vladislav.volodkin/CLionProjects/sapfor")
!11 = !{i32 7, !"Dwarf Version", i32 4}
!12 = !{i32 2, !"Debug Info Version", i32 3}
!13 = !{i32 1, !"wchar_size", i32 4}
!14 = !{i32 7, !"PIC Level", i32 2}
!15 = !{!"clang version 11.1.0 (git@github.com:llvm/llvm-project.git 1fdec59bffc11ae37eb51a1b9869f0696bfd5312)"}
!16 = distinct !DISubprogram(name: "fun1", scope: !10, file: !10, line: 8, type: !17, scopeLine: 8, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2, retainedNodes: !19)
!17 = !DISubroutineType(types: !18)
!18 = !{null, !7}
!19 = !{!20, !21, !22}
!20 = !DILocalVariable(name: "a", arg: 1, scope: !16, file: !10, line: 8, type: !7)
!21 = !DILocalVariable(name: "ptr", scope: !16, file: !10, line: 9, type: !6)
!22 = !DILocalVariable(name: "c", scope: !16, file: !10, line: 13, type: !7)
!23 = !{!24, !24, i64 0}
!24 = !{!"any pointer", !25, i64 0}
!25 = !{!"omnipotent char", !26, i64 0}
!26 = !{!"Simple C/C++ TBAA"}
!27 = !DILocation(line: 8, column: 16, scope: !16)
!28 = !DILocation(line: 9, column: 3, scope: !16)
!29 = !DILocation(line: 9, column: 9, scope: !16)
!30 = !DILocation(line: 10, column: 17, scope: !16)
!31 = !DILocation(line: 10, column: 9, scope: !16)
!32 = !DILocation(line: 10, column: 7, scope: !16)
!33 = !DILocation(line: 11, column: 10, scope: !16)
!34 = !DILocation(line: 11, column: 8, scope: !16)
!35 = !DILocation(line: 13, column: 3, scope: !16)
!36 = !DILocation(line: 13, column: 8, scope: !16)
!37 = !DILocation(line: 13, column: 14, scope: !16)
!38 = !DILocation(line: 14, column: 10, scope: !16)
!39 = !DILocation(line: 14, column: 4, scope: !16)
!40 = !DILocation(line: 14, column: 8, scope: !16)
!41 = !DILocation(line: 15, column: 1, scope: !16)
!42 = distinct !DISubprogram(name: "main", scope: !10, file: !10, line: 17, type: !43, scopeLine: 17, flags: DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2, retainedNodes: !4)
!43 = !DISubroutineType(types: !44)
!44 = !{!8}
!45 = !DILocation(line: 18, column: 3, scope: !42)
