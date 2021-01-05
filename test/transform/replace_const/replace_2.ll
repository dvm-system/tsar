; ModuleID = '/tmp/replace_2-04cc4d.ll'
source_filename = "/tmp/replace_2-04cc4d.ll"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%structd_ = type <{ [404 x i8] }>

@.C301_foo_ = internal constant i32 100
@.C285_foo_ = internal constant i32 1
@d_ = common global %structd_ zeroinitializer, align 64, !dbg !0, !dbg !8

define void @foo_() #0 !dbg !3 {
L.entry:
  %.dY0001_311 = alloca i32, align 4
  %i_298 = alloca i32, align 4
  br label %L.LB1_312

L.LB1_312:                                        ; preds = %L.entry
  store i32 100, i32* %.dY0001_311, align 4, !dbg !22
  call void @llvm.dbg.declare(metadata i32* %i_298, metadata !24, metadata !DIExpression()), !dbg !26
  store i32 1, i32* %i_298, align 4, !dbg !22
  br label %L.LB1_309

L.LB1_309:                                        ; preds = %L.LB1_309, %L.LB1_312
  %0 = load i32, i32* %i_298, align 4, !dbg !27
  call void @llvm.dbg.value(metadata i32 %0, metadata !24, metadata !DIExpression()), !dbg !26
  %1 = sitofp i32 %0 to float, !dbg !27
  %2 = load i32, i32* %i_298, align 4, !dbg !27
  call void @llvm.dbg.value(metadata i32 %2, metadata !24, metadata !DIExpression()), !dbg !26
  %3 = sext i32 %2 to i64, !dbg !27
  %4 = bitcast %structd_* @d_ to float*, !dbg !27
  %5 = getelementptr float, float* %4, i64 %3, !dbg !27
  store float %1, float* %5, align 4, !dbg !27
  %6 = load i32, i32* %i_298, align 4, !dbg !28
  call void @llvm.dbg.value(metadata i32 %6, metadata !24, metadata !DIExpression()), !dbg !26
  %7 = add nsw i32 %6, 1, !dbg !28
  store i32 %7, i32* %i_298, align 4, !dbg !28
  %8 = load i32, i32* %.dY0001_311, align 4, !dbg !28
  %9 = sub nsw i32 %8, 1, !dbg !28
  store i32 %9, i32* %.dY0001_311, align 4, !dbg !28
  %10 = load i32, i32* %.dY0001_311, align 4, !dbg !28
  %11 = icmp sgt i32 %10, 0, !dbg !28
  br i1 %11, label %L.LB1_309, label %L.LB1_322, !dbg !28

L.LB1_322:                                        ; preds = %L.LB1_309
  ret void, !dbg !29
}

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.value(metadata, metadata, metadata) #1

attributes #0 = { "no-frame-pointer-elim-non-leaf" }
attributes #1 = { nounwind readnone speculatable }

!llvm.module.flags = !{!20, !21}
!llvm.dbg.cu = !{!5}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "a", scope: !2, file: !4, type: !16, isLocal: false, isDefinition: true)
!2 = distinct !DICommonBlock(scope: !3, declaration: !9, name: "d")
!3 = distinct !DISubprogram(name: "foo", scope: !5, file: !4, line: 1, type: !14, isLocal: false, isDefinition: true, scopeLine: 1, isOptimized: false, unit: !5)
!4 = !DIFile(filename: "replace_2.f", directory: "/home/kaniandr/workspace/sapfor/analyzers/tsar/test/transform/replace_const")
!5 = distinct !DICompileUnit(language: DW_LANG_Fortran90, file: !4, producer: " F90 Flang - 1.5 2017-05-01", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !6, retainedTypes: !6, globals: !7, imports: !6)
!6 = !{}
!7 = !{!8, !0}
!8 = !DIGlobalVariableExpression(var: !9, expr: !DIExpression())
!9 = distinct !DIGlobalVariable(name: "d", scope: !2, type: !10, isLocal: false, isDefinition: true)
!10 = !DICompositeType(tag: DW_TAG_array_type, baseType: !11, size: 3232, align: 8, elements: !12)
!11 = !DIBasicType(name: "byte", size: 8, align: 8, encoding: DW_ATE_signed)
!12 = !{!13}
!13 = !DISubrange(count: 404)
!14 = !DISubroutineType(types: !15)
!15 = !{null}
!16 = !DICompositeType(tag: DW_TAG_array_type, baseType: !17, size: 3232, align: 32, elements: !18)
!17 = !DIBasicType(name: "real", size: 32, align: 32, encoding: DW_ATE_float)
!18 = !{!19}
!19 = !DISubrange(count: 101, lowerBound: 100)
!20 = !{i32 2, !"Dwarf Version", i32 2}
!21 = !{i32 2, !"Debug Info Version", i32 3}
!22 = !DILocation(line: 7, column: 1, scope: !23)
!23 = !DILexicalBlock(scope: !3, file: !4, line: 1, column: 1)
!24 = !DILocalVariable(name: "i", scope: !23, file: !4, type: !25)
!25 = !DIBasicType(name: "integer", size: 32, align: 32, encoding: DW_ATE_signed)
!26 = !DILocation(line: 0, scope: !23)
!27 = !DILocation(line: 8, column: 1, scope: !23)
!28 = !DILocation(line: 9, column: 1, scope: !23)
!29 = !DILocation(line: 10, column: 1, scope: !23)
