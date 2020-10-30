; ModuleID = '/Users/whcrc/Code/sapfor/analyzers/tsar/test/analysis/address_access/experiments/example.c'
source_filename = "/Users/whcrc/Code/sapfor/analyzers/tsar/test/analysis/address_access/experiments/example.c"
target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx10.15.0"

@glob = global i32* null, align 8, !dbg !0

; Function Attrs: nounwind ssp uwtable
define void @fun1(i32 %a, i32 %b, i32* %ptra) #0 !dbg !15 {
entry:
  %a.addr = alloca i32, align 4
  %b.addr = alloca i32, align 4
  %ptra.addr = alloca i32*, align 8
  %ptr = alloca i32*, align 8
  store i32 %a, i32* %a.addr, align 4, !tbaa !23
  call void @llvm.dbg.declare(metadata i32* %a.addr, metadata !19, metadata !DIExpression()), !dbg !27
  store i32 %b, i32* %b.addr, align 4, !tbaa !23
  call void @llvm.dbg.declare(metadata i32* %b.addr, metadata !20, metadata !DIExpression()), !dbg !28
  store i32* %ptra, i32** %ptra.addr, align 8, !tbaa !29
  call void @llvm.dbg.declare(metadata i32** %ptra.addr, metadata !21, metadata !DIExpression()), !dbg !31
  %0 = bitcast i32** %ptr to i8*, !dbg !32
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %0) #4, !dbg !32
  call void @llvm.dbg.declare(metadata i32** %ptr, metadata !22, metadata !DIExpression()), !dbg !33
  %call = call i8* @malloc(i64 4) #5, !dbg !34
  %1 = bitcast i8* %call to i32*, !dbg !35
  store i32* %1, i32** %ptr, align 8, !dbg !36, !tbaa !29
  %2 = load i32*, i32** %ptr, align 8, !dbg !37, !tbaa !29
  store i32* %2, i32** @glob, align 8, !dbg !38, !tbaa !29
  %3 = load i32, i32* %a.addr, align 4, !dbg !39, !tbaa !23
  %4 = load i32*, i32** %ptr, align 8, !dbg !40, !tbaa !29
  store i32 %3, i32* %4, align 4, !dbg !41, !tbaa !23
  %5 = load i32, i32* %b.addr, align 4, !dbg !42, !tbaa !23
  %6 = load i32*, i32** %ptra.addr, align 8, !dbg !43, !tbaa !29
  store i32 %5, i32* %6, align 4, !dbg !44, !tbaa !23
  %7 = bitcast i32** %ptr to i8*, !dbg !45
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %7) #4, !dbg !45
  ret void, !dbg !45
}

; Function Attrs: nounwind readnone speculatable willreturn
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: argmemonly nounwind willreturn
declare void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture) #2

; Function Attrs: allocsize(0)
declare i8* @malloc(i64) #3

; Function Attrs: argmemonly nounwind willreturn
declare void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture) #2

attributes #0 = { nounwind ssp uwtable "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "frame-pointer"="all" "less-precise-fpmad"="false" "min-legal-vector-width"="0" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind readnone speculatable willreturn }
attributes #2 = { argmemonly nounwind willreturn }
attributes #3 = { allocsize(0) "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "frame-pointer"="all" "less-precise-fpmad"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="penryn" "target-features"="+cx16,+cx8,+fxsr,+mmx,+sahf,+sse,+sse2,+sse3,+sse4.1,+ssse3,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #4 = { nounwind }
attributes #5 = { allocsize(0) }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!10, !11, !12, !13}
!llvm.ident = !{!14}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "glob", scope: !2, file: !9, line: 6, type: !6, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C99, file: !3, producer: "clang version 11.0.0 (https://github.com/llvm/llvm-project.git b3fb40b3a3c1fb7ac094eda50762624baad37552)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, enums: !4, retainedTypes: !5, globals: !8, nameTableKind: None, sysroot: "/")
!3 = !DIFile(filename: "/Users/whcrc/Code/sapfor/analyzers/tsar/test/analysis/address_access/experiments/example.c", directory: "/Users/whcrc/Code/sapfor/analyzers/tsar/test/analysis/address_access/experiments")
!4 = !{}
!5 = !{!6}
!6 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !7, size: 64)
!7 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!8 = !{!0}
!9 = !DIFile(filename: "example.c", directory: "/Users/whcrc/Code/sapfor/analyzers/tsar/test/analysis/address_access/experiments")
!10 = !{i32 7, !"Dwarf Version", i32 4}
!11 = !{i32 2, !"Debug Info Version", i32 3}
!12 = !{i32 1, !"wchar_size", i32 4}
!13 = !{i32 7, !"PIC Level", i32 2}
!14 = !{!"clang version 11.0.0 (https://github.com/llvm/llvm-project.git b3fb40b3a3c1fb7ac094eda50762624baad37552)"}
!15 = distinct !DISubprogram(name: "fun1", scope: !9, file: !9, line: 8, type: !16, scopeLine: 8, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !2, retainedNodes: !18)
!16 = !DISubroutineType(types: !17)
!17 = !{null, !7, !7, !6}
!18 = !{!19, !20, !21, !22}
!19 = !DILocalVariable(name: "a", arg: 1, scope: !15, file: !9, line: 8, type: !7)
!20 = !DILocalVariable(name: "b", arg: 2, scope: !15, file: !9, line: 8, type: !7)
!21 = !DILocalVariable(name: "ptra", arg: 3, scope: !15, file: !9, line: 8, type: !6)
!22 = !DILocalVariable(name: "ptr", scope: !15, file: !9, line: 9, type: !6)
!23 = !{!24, !24, i64 0}
!24 = !{!"int", !25, i64 0}
!25 = !{!"omnipotent char", !26, i64 0}
!26 = !{!"Simple C/C++ TBAA"}
!27 = !DILocation(line: 8, column: 15, scope: !15)
!28 = !DILocation(line: 8, column: 22, scope: !15)
!29 = !{!30, !30, i64 0}
!30 = !{!"any pointer", !25, i64 0}
!31 = !DILocation(line: 8, column: 30, scope: !15)
!32 = !DILocation(line: 9, column: 3, scope: !15)
!33 = !DILocation(line: 9, column: 8, scope: !15)
!34 = !DILocation(line: 10, column: 16, scope: !15)
!35 = !DILocation(line: 10, column: 9, scope: !15)
!36 = !DILocation(line: 10, column: 7, scope: !15)
!37 = !DILocation(line: 11, column: 10, scope: !15)
!38 = !DILocation(line: 11, column: 8, scope: !15)
!39 = !DILocation(line: 13, column: 10, scope: !15)
!40 = !DILocation(line: 13, column: 4, scope: !15)
!41 = !DILocation(line: 13, column: 8, scope: !15)
!42 = !DILocation(line: 14, column: 11, scope: !15)
!43 = !DILocation(line: 14, column: 4, scope: !15)
!44 = !DILocation(line: 14, column: 9, scope: !15)
!45 = !DILocation(line: 15, column: 1, scope: !15)
