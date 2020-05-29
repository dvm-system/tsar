unsigned x;

void foo(float *a, int n) {
  for (int i = 0; i < n; ++i) {
    x += i;
    *a += x;
  }
}
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: nounwind uwtable
//CHECK: define dso_local void @foo(float* %a, i32 %n) #0 !dbg !12 !alias.tree !23 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata float* %a, metadata !19, metadata !DIExpression()), !dbg !29
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !20, metadata !DIExpression()), !dbg !39
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !21, metadata !DIExpression()), !dbg !26
//CHECK:   br label %for.cond, !dbg !42
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.body, %entry
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.body ], !dbg !43
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !21, metadata !DIExpression()), !dbg !26
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !44
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !45
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   ret void, !dbg !46
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %0 = load i32, i32* @x, align 4, !dbg !47, !tbaa !48
//CHECK:   %add = add i32 %0, %i.0, !dbg !47
//CHECK:   store i32 %add, i32* @x, align 4, !dbg !47, !tbaa !48
//CHECK:   %1 = load i32, i32* @x, align 4, !dbg !52, !tbaa !48
//CHECK:   %conv = uitofp i32 %1 to float, !dbg !52
//CHECK:   %2 = load float, float* %a, align 4, !dbg !53, !tbaa !54
//CHECK:   %add1 = fadd float %2, %conv, !dbg !53
//CHECK:   store float %add1, float* %a, align 4, !dbg !53, !tbaa !54
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !56
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !21, metadata !DIExpression()), !dbg !26
//CHECK:   br label %for.cond, !dbg !57, !llvm.loop !58
//CHECK: }
//CHECK: now working with x
//CHECK: validated
//CHECK: analyzed
//CHECK: creating metadata for global
//CHECK: loads inserted
//CHECK: phis inserted
//CHECK: loads handled
//CHECK: done
//CHECK: now working with a
//CHECK: validated
//CHECK: analyzed
//CHECK: dbg value call found
//CHECK: loads inserted
//CHECK: phis inserted
//CHECK: loads handled
//CHECK: done
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: nounwind uwtable
//CHECK: define dso_local void @foo(float* %a, i32 %n) #0 !dbg !12 !alias.tree !23 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata float* %a, metadata !19, metadata !DIExpression()), !dbg !29
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !20, metadata !DIExpression()), !dbg !39
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !21, metadata !DIExpression()), !dbg !26
//CHECK:   %load.x = load i32, i32* @x
//CHECK:   %load.a = load float, float* %a
//CHECK:   br label %for.cond, !dbg !42
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.body, %entry
//CHECK:   %phi.for.cond5 = phi float [ %add1, %for.body ], [ %load.a, %entry ]
//CHECK:   %phi.for.cond = phi i32 [ %add, %for.body ], [ %load.x, %entry ]
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.body ], !dbg !43
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !21, metadata !DIExpression()), !dbg !26
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !44
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !45
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   store float %phi.for.cond5, float* %a
//CHECK:   store i32 %phi.for.cond, i32* @x
//CHECK:   ret void, !dbg !46
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %add = add i32 %phi.for.cond, %i.0, !dbg !47
//CHECK:   %conv = uitofp i32 %add to float, !dbg !48
//CHECK:   %add1 = fadd float %phi.for.cond5, %conv, !dbg !49
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !50
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !21, metadata !DIExpression()), !dbg !26
//CHECK:   br label %for.cond, !dbg !51, !llvm.loop !52
//CHECK: }
