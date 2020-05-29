void sum(double *x, int n) {
  for (int i = 0; i < n; ++i) {
    *x *= i;
  }
}
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: argmemonly nounwind uwtable
//CHECK: define dso_local void @sum(double* nocapture %x, i32 %n) #0 !dbg !7 !alias.tree !19 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata double* %x, metadata !15, metadata !DIExpression()), !dbg !22
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !16, metadata !DIExpression()), !dbg !25
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !17, metadata !DIExpression()), !dbg !30
//CHECK:   br label %for.cond, !dbg !36
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.body, %entry
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.body ], !dbg !37
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !17, metadata !DIExpression()), !dbg !30
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !38
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !39
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   ret void, !dbg !40
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %conv = sitofp i32 %i.0 to double, !dbg !41
//CHECK:   %0 = load double, double* %x, align 8, !dbg !42, !tbaa !43
//CHECK:   %mul = fmul double %0, %conv, !dbg !42
//CHECK:   store double %mul, double* %x, align 8, !dbg !42, !tbaa !43
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !47
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !17, metadata !DIExpression()), !dbg !30
//CHECK:   br label %for.cond, !dbg !48, !llvm.loop !49
//CHECK: }
//CHECK: now working with x
//CHECK: validated
//CHECK: analyzed
//CHECK: dbg value call found
//CHECK: loads inserted
//CHECK: phis inserted
//CHECK: loads handled
//CHECK: done
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: argmemonly nounwind uwtable
//CHECK: define dso_local void @sum(double* nocapture %x, i32 %n) #0 !dbg !7 !alias.tree !19 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata double* %x, metadata !15, metadata !DIExpression()), !dbg !22
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !16, metadata !DIExpression()), !dbg !25
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !17, metadata !DIExpression()), !dbg !30
//CHECK:   %load.x = load double, double* %x
//CHECK:   br label %for.cond, !dbg !36
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.body, %entry
//CHECK:   %phi.for.cond = phi double [ %mul, %for.body ], [ %load.x, %entry ]
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.body ], !dbg !37
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !17, metadata !DIExpression()), !dbg !30
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !38
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !39
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   store double %phi.for.cond, double* %x
//CHECK:   ret void, !dbg !40
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %conv = sitofp i32 %i.0 to double, !dbg !41
//CHECK:   %mul = fmul double %phi.for.cond, %conv, !dbg !42
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !43
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !17, metadata !DIExpression()), !dbg !30
//CHECK:   br label %for.cond, !dbg !44, !llvm.loop !45
//CHECK: }
