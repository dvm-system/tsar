; ModuleID = '/tmp/replace_4-945d7b.ll'
source_filename = "/tmp/replace_4-945d7b.ll"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.BSS1 = type <{ [24 x i8] }>

@.BSS1 = internal global %struct.BSS1 zeroinitializer, align 32, !dbg !0
@.C301_MAIN_ = internal constant double 3.000000e+00
@.C299_MAIN_ = internal constant i64 3
@.C293_MAIN_ = internal constant double 2.000000e+00
@.C300_MAIN_ = internal constant i64 2
@.C292_MAIN_ = internal constant double 1.000000e+00
@.C286_MAIN_ = internal constant i64 1
@.C283_MAIN_ = internal constant i32 0

define void @MAIN_() #0 !dbg !2 {
L.entry:
  %0 = bitcast i32* @.C283_MAIN_ to i8*, !dbg !15
  %1 = bitcast void (...)* @fort_init to void (i8*, ...)*, !dbg !15
  call void (i8*, ...) %1(i8* %0), !dbg !15
  br label %L.LB1_306

L.LB1_306:                                        ; preds = %L.entry
  %2 = bitcast %struct.BSS1* @.BSS1 to double*, !dbg !17
  store double 1.000000e+00, double* %2, align 8, !dbg !17
  %3 = bitcast %struct.BSS1* @.BSS1 to i8*, !dbg !18
  %4 = getelementptr i8, i8* %3, i64 8, !dbg !18
  %5 = bitcast i8* %4 to double*, !dbg !18
  store double 2.000000e+00, double* %5, align 8, !dbg !18
  %6 = bitcast %struct.BSS1* @.BSS1 to i8*, !dbg !19
  %7 = getelementptr i8, i8* %6, i64 16, !dbg !19
  %8 = bitcast i8* %7 to double*, !dbg !19
  store double 3.000000e+00, double* %8, align 8, !dbg !19
  ret void, !dbg !20
}

declare void @fort_init(...) #0

attributes #0 = { "no-frame-pointer-elim-non-leaf" }

!llvm.module.flags = !{!13, !14}
!llvm.dbg.cu = !{!4}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "a", scope: !2, file: !3, type: !9, isLocal: true, isDefinition: true)
!2 = distinct !DISubprogram(name: "eliminate", scope: !4, file: !3, line: 1, type: !7, isLocal: false, isDefinition: true, scopeLine: 1, isOptimized: false, unit: !4)
!3 = !DIFile(filename: "replace_4.f", directory: "/home/kaniandr/workspace/sapfor/analyzers/tsar/test/transform/replace_const")
!4 = distinct !DICompileUnit(language: DW_LANG_Fortran90, file: !3, producer: " F90 Flang - 1.5 2017-05-01", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !5, retainedTypes: !5, globals: !6, imports: !5)
!5 = !{}
!6 = !{!0}
!7 = !DISubroutineType(cc: DW_CC_program, types: !8)
!8 = !{null}
!9 = !DICompositeType(tag: DW_TAG_array_type, baseType: !10, size: 192, align: 64, elements: !11)
!10 = !DIBasicType(name: "double precision", size: 64, align: 64, encoding: DW_ATE_float)
!11 = !{!12}
!12 = !DISubrange(count: 3, lowerBound: 1)
!13 = !{i32 2, !"Dwarf Version", i32 2}
!14 = !{i32 2, !"Debug Info Version", i32 3}
!15 = !DILocation(line: 1, column: 1, scope: !16)
!16 = !DILexicalBlock(scope: !2, file: !3, line: 1, column: 1)
!17 = !DILocation(line: 3, column: 1, scope: !16)
!18 = !DILocation(line: 4, column: 1, scope: !16)
!19 = !DILocation(line: 5, column: 1, scope: !16)
!20 = !DILocation(line: 6, column: 1, scope: !16)
