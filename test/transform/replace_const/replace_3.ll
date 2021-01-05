; ModuleID = '/tmp/replace_3-63e71e.ll'
source_filename = "/tmp/replace_3-63e71e.ll"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct_baz_0_ = type <{ [160 x i8] }>

@.C300_bar_ = internal constant float 5.000000e+00
@.C302_baz_foo_ = internal constant i64 4
@.C285_baz_foo_ = internal constant i32 1
@.C310_baz_foo_ = internal constant i64 3
@.C283_MAIN_ = internal constant i32 0
@_baz_0_ = common global %struct_baz_0_ zeroinitializer, align 64, !dbg !0

define void @bar_(i64* %integerx) #0 !dbg !14 {
L.entry:
  %x_299 = alloca float, align 4
  call void @llvm.dbg.declare(metadata i64* %integerx, metadata !17, metadata !DIExpression()), !dbg !18
  br label %L.LB1_304

L.LB1_304:                                        ; preds = %L.entry
  call void @llvm.dbg.declare(metadata float* %x_299, metadata !20, metadata !DIExpression()), !dbg !18
  store float 5.000000e+00, float* %x_299, align 4, !dbg !22
  ret void, !dbg !23
}

; Function Attrs: noinline
define float @baz_() #1 {
.L.entry:
  ret float undef
}

define void @baz_foo_(i64* %integeri) #0 !dbg !24 {
L.entry:
  %i_309 = alloca i32, align 4
  %i2_0_311 = alloca i32, align 4
  call void @llvm.dbg.declare(metadata i64* %integeri, metadata !25, metadata !DIExpression()), !dbg !26
  br label %L.LB3_315

L.LB3_315:                                        ; preds = %L.entry
  %0 = bitcast %struct_baz_0_* @_baz_0_ to i8*, !dbg !28
  %1 = getelementptr i8, i8* %0, i64 36, !dbg !28
  call void @llvm.dbg.declare(metadata i32* %i_309, metadata !29, metadata !DIExpression()), !dbg !26
  %2 = load i32, i32* %i_309, align 4, !dbg !28
  call void @llvm.dbg.value(metadata i32 %2, metadata !29, metadata !DIExpression()), !dbg !26
  %3 = sext i32 %2 to i64, !dbg !28
  %4 = mul nsw i64 %3, 4, !dbg !28
  %5 = getelementptr i8, i8* %1, i64 %4, !dbg !28
  %6 = bitcast i8* %5 to i64*, !dbg !28
  call void @bar_(i64* %6), !dbg !28
  %7 = bitcast %struct_baz_0_* @_baz_0_ to i8*, !dbg !30
  %8 = getelementptr i8, i8* %7, i64 92, !dbg !30
  %9 = bitcast i8* %8 to i32*, !dbg !30
  %10 = load i32, i32* %9, align 4, !dbg !30
  %11 = load i32, i32* %i_309, align 4, !dbg !30
  call void @llvm.dbg.value(metadata i32 %11, metadata !29, metadata !DIExpression()), !dbg !26
  %12 = sext i32 %11 to i64, !dbg !30
  call void @llvm.dbg.declare(metadata i32* %i2_0_311, metadata !31, metadata !DIExpression()), !dbg !26
  %13 = load i32, i32* %i2_0_311, align 4, !dbg !30
  call void @llvm.dbg.value(metadata i32 %13, metadata !31, metadata !DIExpression()), !dbg !26
  %14 = sext i32 %13 to i64, !dbg !30
  %15 = mul nsw i64 %14, 10, !dbg !30
  %16 = add nsw i64 %12, %15, !dbg !30
  %17 = bitcast %struct_baz_0_* @_baz_0_ to i8*, !dbg !30
  %18 = getelementptr i8, i8* %17, i64 -88, !dbg !30
  %19 = bitcast i8* %18 to i32*, !dbg !30
  %20 = getelementptr i32, i32* %19, i64 %16, !dbg !30
  %21 = load i32, i32* %20, align 4, !dbg !30
  %22 = add nsw i32 %10, %21, !dbg !30
  %23 = load i32, i32* %i_309, align 4, !dbg !30
  call void @llvm.dbg.value(metadata i32 %23, metadata !29, metadata !DIExpression()), !dbg !26
  %24 = sext i32 %23 to i64, !dbg !30
  %25 = bitcast %struct_baz_0_* @_baz_0_ to i8*, !dbg !30
  %26 = getelementptr i8, i8* %25, i64 36, !dbg !30
  %27 = bitcast i8* %26 to i32*, !dbg !30
  %28 = getelementptr i32, i32* %27, i64 %24, !dbg !30
  store i32 %22, i32* %28, align 4, !dbg !30
  ret void, !dbg !32
}

define void @MAIN_() #0 !dbg !33 {
L.entry:
  %0 = bitcast i32* @.C283_MAIN_ to i8*, !dbg !36
  %1 = bitcast void (...)* @fort_init to void (i8*, ...)*, !dbg !36
  call void (i8*, ...) %1(i8* %0), !dbg !36
  br label %L.LB4_302

L.LB4_302:                                        ; preds = %L.entry
  ret void, !dbg !36
}

declare void @fort_init(...) #0

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.declare(metadata, metadata, metadata) #2

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.value(metadata, metadata, metadata) #2

attributes #0 = { "no-frame-pointer-elim-non-leaf" }
attributes #1 = { noinline }
attributes #2 = { nounwind readnone speculatable }

!llvm.module.flags = !{!12, !13}
!llvm.dbg.cu = !{!3}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "a", scope: !2, file: !4, type: !7, isLocal: false, isDefinition: true)
!2 = !DIModule(scope: !3, name: "baz")
!3 = distinct !DICompileUnit(language: DW_LANG_Fortran90, file: !4, producer: " F90 Flang - 1.5 2017-05-01", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !5, retainedTypes: !5, globals: !6, imports: !5)
!4 = !DIFile(filename: "replace_3.f", directory: "/home/kaniandr/workspace/sapfor/analyzers/tsar/test/transform/replace_const")
!5 = !{}
!6 = !{!0}
!7 = !DICompositeType(tag: DW_TAG_array_type, baseType: !8, size: 1280, align: 32, elements: !9)
!8 = !DIBasicType(name: "integer", size: 32, align: 32, encoding: DW_ATE_signed)
!9 = !{!10, !11}
!10 = !DISubrange(count: 10, lowerBound: 1)
!11 = !DISubrange(count: 4, lowerBound: 2)
!12 = !{i32 2, !"Dwarf Version", i32 2}
!13 = !{i32 2, !"Debug Info Version", i32 3}
!14 = distinct !DISubprogram(name: "bar", scope: !3, file: !4, line: 1, type: !15, isLocal: false, isDefinition: true, scopeLine: 1, isOptimized: false, unit: !3)
!15 = !DISubroutineType(types: !16)
!16 = !{null, !8}
!17 = !DILocalVariable(name: "integerx", arg: 1, scope: !14, file: !4, type: !8)
!18 = !DILocation(line: 0, scope: !19)
!19 = !DILexicalBlock(scope: !14, file: !4, line: 1, column: 1)
!20 = !DILocalVariable(name: "x", scope: !19, file: !4, type: !21)
!21 = !DIBasicType(name: "real", size: 32, align: 32, encoding: DW_ATE_float)
!22 = !DILocation(line: 2, column: 1, scope: !19)
!23 = !DILocation(line: 3, column: 1, scope: !19)
!24 = distinct !DISubprogram(name: "foo", scope: !2, file: !4, line: 10, type: !15, isLocal: false, isDefinition: true, scopeLine: 10, isOptimized: false, unit: !3)
!25 = !DILocalVariable(name: "integeri", arg: 1, scope: !24, file: !4, type: !8)
!26 = !DILocation(line: 0, scope: !27)
!27 = !DILexicalBlock(scope: !24, file: !4, line: 10, column: 1)
!28 = !DILocation(line: 11, column: 1, scope: !27)
!29 = !DILocalVariable(name: "i", scope: !27, file: !4, type: !8)
!30 = !DILocation(line: 12, column: 1, scope: !27)
!31 = !DILocalVariable(name: "i2_0", scope: !27, file: !4, type: !8)
!32 = !DILocation(line: 13, column: 1, scope: !27)
!33 = distinct !DISubprogram(name: "MAIN", scope: !3, file: !4, line: 15, type: !34, isLocal: false, isDefinition: true, scopeLine: 15, isOptimized: false, unit: !3)
!34 = !DISubroutineType(cc: DW_CC_program, types: !35)
!35 = !{null}
!36 = !DILocation(line: 15, column: 1, scope: !37)
!37 = !DILexicalBlock(scope: !33, file: !4, line: 15, column: 1)
