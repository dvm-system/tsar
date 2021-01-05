; ModuleID = '/tmp/replace_1-6e8fc4.ll'
source_filename = "/tmp/replace_1-6e8fc4.ll"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

%struct.BSS1 = type <{ [40 x i8] }>

@.BSS1 = internal global %struct.BSS1 zeroinitializer, align 32, !dbg !0
@.C300_foo_ = internal constant i64 9
@.C304_foo_ = internal constant i32 8
@.C283_foo_ = internal constant i32 0
@.C303_foo_ = internal constant i32 10
@.C285_foo_ = internal constant i32 1

define signext i32 @foo_() #0 !dbg !2 {
L.entry:
  %.dY0001_309 = alloca i32, align 4
  %i_302 = alloca i32, align 4
  %foo_298 = alloca i32, align 4
  %.dY0002_312 = alloca i32, align 4
  br label %L.LB1_313

L.LB1_313:                                        ; preds = %L.entry
  store i32 10, i32* %.dY0001_309, align 4, !dbg !17
  call void @llvm.dbg.declare(metadata i32* %i_302, metadata !19, metadata !DIExpression()), !dbg !20
  store i32 1, i32* %i_302, align 4, !dbg !17
  br label %L.LB1_307

L.LB1_307:                                        ; preds = %L.LB1_307, %L.LB1_313
  %0 = load i32, i32* %i_302, align 4, !dbg !21
  call void @llvm.dbg.value(metadata i32 %0, metadata !19, metadata !DIExpression()), !dbg !20
  %1 = sitofp i32 %0 to float, !dbg !21
  %2 = load i32, i32* %i_302, align 4, !dbg !21
  call void @llvm.dbg.value(metadata i32 %2, metadata !19, metadata !DIExpression()), !dbg !20
  %3 = sext i32 %2 to i64, !dbg !21
  %4 = bitcast %struct.BSS1* @.BSS1 to i8*, !dbg !21
  %5 = getelementptr i8, i8* %4, i64 -4, !dbg !21
  %6 = bitcast i8* %5 to float*, !dbg !21
  %7 = getelementptr float, float* %6, i64 %3, !dbg !21
  store float %1, float* %7, align 4, !dbg !21
  %8 = load i32, i32* %i_302, align 4, !dbg !22
  call void @llvm.dbg.value(metadata i32 %8, metadata !19, metadata !DIExpression()), !dbg !20
  %9 = add nsw i32 %8, 1, !dbg !22
  store i32 %9, i32* %i_302, align 4, !dbg !22
  %10 = load i32, i32* %.dY0001_309, align 4, !dbg !22
  %11 = sub nsw i32 %10, 1, !dbg !22
  store i32 %11, i32* %.dY0001_309, align 4, !dbg !22
  %12 = load i32, i32* %.dY0001_309, align 4, !dbg !22
  %13 = icmp sgt i32 %12, 0, !dbg !22
  br i1 %13, label %L.LB1_307, label %L.LB1_336, !dbg !22

L.LB1_336:                                        ; preds = %L.LB1_307
  call void @llvm.dbg.declare(metadata i32* %foo_298, metadata !23, metadata !DIExpression()), !dbg !20
  store i32 0, i32* %foo_298, align 4, !dbg !24
  store i32 9, i32* %.dY0002_312, align 4, !dbg !25
  store i32 0, i32* %i_302, align 4, !dbg !25
  br label %L.LB1_310

L.LB1_310:                                        ; preds = %L.LB1_310, %L.LB1_336
  %14 = load i32, i32* %foo_298, align 4, !dbg !26
  call void @llvm.dbg.value(metadata i32 %14, metadata !23, metadata !DIExpression()), !dbg !20
  %15 = sitofp i32 %14 to float, !dbg !26
  %16 = load i32, i32* %i_302, align 4, !dbg !26
  call void @llvm.dbg.value(metadata i32 %16, metadata !19, metadata !DIExpression()), !dbg !20
  %17 = sext i32 %16 to i64, !dbg !26
  %18 = bitcast %struct.BSS1* @.BSS1 to float*, !dbg !26
  %19 = getelementptr float, float* %18, i64 %17, !dbg !26
  %20 = load float, float* %19, align 4, !dbg !26
  %21 = fadd fast float %15, %20, !dbg !26
  %22 = fptosi float %21 to i32, !dbg !26
  store i32 %22, i32* %foo_298, align 4, !dbg !26
  %23 = load i32, i32* %i_302, align 4, !dbg !27
  call void @llvm.dbg.value(metadata i32 %23, metadata !19, metadata !DIExpression()), !dbg !20
  %24 = sext i32 %23 to i64, !dbg !27
  %25 = bitcast %struct.BSS1* @.BSS1 to float*, !dbg !27
  %26 = getelementptr float, float* %25, i64 %24, !dbg !27
  %27 = load float, float* %26, align 4, !dbg !27
  %28 = load i32, i32* %i_302, align 4, !dbg !27
  call void @llvm.dbg.value(metadata i32 %28, metadata !19, metadata !DIExpression()), !dbg !20
  %29 = sext i32 %28 to i64, !dbg !27
  %30 = bitcast %struct.BSS1* @.BSS1 to i8*, !dbg !27
  %31 = getelementptr i8, i8* %30, i64 4, !dbg !27
  %32 = bitcast i8* %31 to float*, !dbg !27
  %33 = getelementptr float, float* %32, i64 %29, !dbg !27
  %34 = load float, float* %33, align 4, !dbg !27
  %35 = fadd fast float %27, %34, !dbg !27
  %36 = load i32, i32* %i_302, align 4, !dbg !27
  call void @llvm.dbg.value(metadata i32 %36, metadata !19, metadata !DIExpression()), !dbg !20
  %37 = sext i32 %36 to i64, !dbg !27
  %38 = bitcast %struct.BSS1* @.BSS1 to i8*, !dbg !27
  %39 = getelementptr i8, i8* %38, i64 4, !dbg !27
  %40 = bitcast i8* %39 to float*, !dbg !27
  %41 = getelementptr float, float* %40, i64 %37, !dbg !27
  store float %35, float* %41, align 4, !dbg !27
  %42 = load i32, i32* %i_302, align 4, !dbg !28
  call void @llvm.dbg.value(metadata i32 %42, metadata !19, metadata !DIExpression()), !dbg !20
  %43 = add nsw i32 %42, 1, !dbg !28
  store i32 %43, i32* %i_302, align 4, !dbg !28
  %44 = load i32, i32* %.dY0002_312, align 4, !dbg !28
  %45 = sub nsw i32 %44, 1, !dbg !28
  store i32 %45, i32* %.dY0002_312, align 4, !dbg !28
  %46 = load i32, i32* %.dY0002_312, align 4, !dbg !28
  %47 = icmp sgt i32 %46, 0, !dbg !28
  br i1 %47, label %L.LB1_310, label %L.LB1_337, !dbg !28

L.LB1_337:                                        ; preds = %L.LB1_310
  %48 = load i32, i32* %foo_298, align 4, !dbg !29
  call void @llvm.dbg.value(metadata i32 %48, metadata !23, metadata !DIExpression()), !dbg !20
  %49 = sitofp i32 %48 to float, !dbg !29
  %50 = bitcast %struct.BSS1* @.BSS1 to i8*, !dbg !29
  %51 = getelementptr i8, i8* %50, i64 36, !dbg !29
  %52 = bitcast i8* %51 to float*, !dbg !29
  %53 = load float, float* %52, align 4, !dbg !29
  %54 = fadd fast float %49, %53, !dbg !29
  %55 = fptosi float %54 to i32, !dbg !29
  store i32 %55, i32* %foo_298, align 4, !dbg !29
  %56 = load i32, i32* %foo_298, align 4, !dbg !30
  call void @llvm.dbg.value(metadata i32 %56, metadata !23, metadata !DIExpression()), !dbg !20
  ret i32 %56, !dbg !30
}

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.declare(metadata, metadata, metadata) #1

; Function Attrs: nounwind readnone speculatable
declare void @llvm.dbg.value(metadata, metadata, metadata) #1

attributes #0 = { "no-frame-pointer-elim-non-leaf" }
attributes #1 = { nounwind readnone speculatable }

!llvm.module.flags = !{!15, !16}
!llvm.dbg.cu = !{!4}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = distinct !DIGlobalVariable(name: "a", scope: !2, file: !3, type: !11, isLocal: true, isDefinition: true)
!2 = distinct !DISubprogram(name: "foo", scope: !4, file: !3, line: 1, type: !7, isLocal: false, isDefinition: true, scopeLine: 1, isOptimized: false, unit: !4)
!3 = !DIFile(filename: "replace_1.f", directory: "/home/kaniandr/workspace/sapfor/analyzers/tsar/test/transform/replace_const")
!4 = distinct !DICompileUnit(language: DW_LANG_Fortran90, file: !3, producer: " F90 Flang - 1.5 2017-05-01", isOptimized: false, runtimeVersion: 0, emissionKind: FullDebug, enums: !5, retainedTypes: !5, globals: !6, imports: !5)
!5 = !{}
!6 = !{!0}
!7 = !DISubroutineType(types: !8)
!8 = !{!9}
!9 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !10, size: 64, align: 64)
!10 = !DIBasicType(name: "integer", size: 32, align: 32, encoding: DW_ATE_signed)
!11 = !DICompositeType(tag: DW_TAG_array_type, baseType: !12, size: 320, align: 32, elements: !13)
!12 = !DIBasicType(name: "real", size: 32, align: 32, encoding: DW_ATE_float)
!13 = !{!14}
!14 = !DISubrange(count: 10)
!15 = !{i32 2, !"Dwarf Version", i32 2}
!16 = !{i32 2, !"Debug Info Version", i32 3}
!17 = !DILocation(line: 4, column: 1, scope: !18)
!18 = !DILexicalBlock(scope: !2, file: !3, line: 1, column: 1)
!19 = !DILocalVariable(name: "i", scope: !18, file: !3, type: !10)
!20 = !DILocation(line: 0, scope: !18)
!21 = !DILocation(line: 5, column: 1, scope: !18)
!22 = !DILocation(line: 6, column: 1, scope: !18)
!23 = !DILocalVariable(scope: !18, file: !3, type: !10, flags: DIFlagArtificial)
!24 = !DILocation(line: 7, column: 1, scope: !18)
!25 = !DILocation(line: 8, column: 1, scope: !18)
!26 = !DILocation(line: 9, column: 1, scope: !18)
!27 = !DILocation(line: 10, column: 1, scope: !18)
!28 = !DILocation(line: 11, column: 1, scope: !18)
!29 = !DILocation(line: 12, column: 1, scope: !18)
!30 = !DILocation(line: 13, column: 1, scope: !18)
