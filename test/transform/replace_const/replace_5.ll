; ModuleID = '/tmp/replace_5-70c13f.ll'
source_filename = "/tmp/replace_5-70c13f.ll"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.BSS1 = type <{ [40 x i8] }>

@.BSS1 = internal global %struct.BSS1 zeroinitializer, align 32, !dbg !0
@.C299_foo_ = internal constant i32 10
@.C285_foo_ = internal constant i32 1
@.C302_foo_ = internal constant i32 -1

define void @foo_() #0 !dbg !2 {
L.entry:
  %i_301 = alloca i32, align 4
  br label %L.LB1_307

L.LB1_307:                                        ; preds = %L.entry
  call void @llvm.dbg.declare(metadata i32* %i_301, metadata !15, metadata !DIExpression()), !dbg !17
  store i32 -1, i32* %i_301, align 4, !dbg !18
  br label %L.LB1_303

L.LB1_303:                                        ; preds = %L.LB1_315, %L.LB1_307
  %0 = load i32, i32* %i_301, align 4, !dbg !18
  call void @llvm.dbg.value(metadata i32 %0, metadata !15, metadata !DIExpression()), !dbg !17
  %1 = load i32, i32* %i_301, align 4, !dbg !18
  call void @llvm.dbg.value(metadata i32 %1, metadata !15, metadata !DIExpression()), !dbg !17
  %2 = sext i32 %1 to i64, !dbg !18
  %3 = bitcast %struct.BSS1* @.BSS1 to i32*, !dbg !18
  %4 = getelementptr i32, i32* %3, i64 %2, !dbg !18
  store i32 %0, i32* %4, align 4, !dbg !18
  %5 = load i32, i32* %i_301, align 4, !dbg !18
  call void @llvm.dbg.value(metadata i32 %5, metadata !15, metadata !DIExpression()), !dbg !17
  %6 = icmp sge i32 %5, 10, !dbg !18
  br i1 %6, label %L.LB1_306, label %L.LB1_315, !dbg !18

L.LB1_315:                                        ; preds = %L.LB1_303
  br label %L.LB1_303, !dbg !18

L.LB1_306:                                        ; preds = %L.LB1_303
  ret void, !dbg !19
}

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.value(metadata, metadata, metadata) #1

attributes #0 = { "no-frame-pointer-elim-non-leaf" }
attributes #1 = { nounwind readnone speculatable }

!llvm.module.flags = !{!13, !14}
!llvm.dbg.cu = !{!4}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "a", scope: !2, file: !3, type: !9, isLocal: true, isDefinition: true)
!2 = distinct !DISubprogram(name: "foo", scope: !4, file: !3, line: 1, type: !7, isLocal: false, isDefinition: true, scopeLine: 1, isOptimized: false, unit: !4)
!3 = !DIFile(filename: "replace_5.f90", directory: "/home/kaniandr/workspace/sapfor/analyzers/tsar/test/transform/replace_const")
!4 = distinct !DICompileUnit(language: DW_LANG_Fortran90, file: !3, producer: " F90 Flang - 1.5 2017-05-01", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !5, retainedTypes: !5, globals: !6, imports: !5)
!5 = !{}
!6 = !{!0}
!7 = !DISubroutineType(types: !8)
!8 = !{null}
!9 = !DICompositeType(tag: DW_TAG_array_type, baseType: !10, size: 320, align: 32, elements: !11)
!10 = !DIBasicType(name: "integer", size: 32, align: 32, encoding: DW_ATE_signed)
!11 = !{!12}
!12 = !DISubrange(count: 10, lowerBound: 1)
!13 = !{i32 2, !"Dwarf Version", i32 2}
!14 = !{i32 2, !"Debug Info Version", i32 3}
!15 = !DILocalVariable(name: "i", scope: !16, file: !3, type: !10)
!16 = !DILexicalBlock(scope: !2, file: !3, line: 1, column: 1)
!17 = !DILocation(line: 0, scope: !16)
!18 = !DILocation(line: 2, column: 1, scope: !16)
!19 = !DILocation(line: 3, column: 1, scope: !16)
