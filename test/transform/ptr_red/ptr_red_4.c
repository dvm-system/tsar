double foo(double x, int n, double **ptr) {
  for (int i = 0; i < n; ++i) {
    x *= i;
    x += 3;
    if (i > 10) {
      x /= 10;
    } else {
      x /= i;
    }
    for (int j = 0; j < 10; ++j) {
      x += j;
    }
  }
  *ptr = &x;
  return x;
}
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: nounwind uwtable
//CHECK: define dso_local double @foo(double %x, i32 %n, double** %ptr) #0 !dbg !7 !alias.tree !25 {
//CHECK: entry:
//CHECK:   %x.addr = alloca double, align 8
//CHECK:   store double %x, double* %x.addr, align 8, !tbaa !46
//CHECK:   call void @llvm.dbg.declare(metadata double* %x.addr, metadata !16, metadata !DIExpression()), !dbg !42
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !17, metadata !DIExpression()), !dbg !45
//CHECK:   call void @llvm.dbg.value(metadata double** %ptr, metadata !18, metadata !DIExpression()), !dbg !29
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !19, metadata !DIExpression()), !dbg !34
//CHECK:   br label %for.cond, !dbg !50
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.cond.cleanup8, %entry
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc13, %for.cond.cleanup8 ], !dbg !51
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !19, metadata !DIExpression()), !dbg !34
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !52
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !53
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   store double* %x.addr, double** %ptr, align 8, !dbg !54, !tbaa !55
//CHECK:   %0 = load double, double* %x.addr, align 8, !dbg !57, !tbaa !46
//CHECK:   ret double %0, !dbg !58
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %conv = sitofp i32 %i.0 to double, !dbg !59
//CHECK:   %1 = load double, double* %x.addr, align 8, !dbg !60, !tbaa !46
//CHECK:   %mul = fmul double %1, %conv, !dbg !60
//CHECK:   store double %mul, double* %x.addr, align 8, !dbg !60, !tbaa !46
//CHECK:   %2 = load double, double* %x.addr, align 8, !dbg !61, !tbaa !46
//CHECK:   %add = fadd double %2, 3.000000e+00, !dbg !61
//CHECK:   store double %add, double* %x.addr, align 8, !dbg !61, !tbaa !46
//CHECK:   %cmp1 = icmp sgt i32 %i.0, 10, !dbg !62
//CHECK:   br i1 %cmp1, label %if.then, label %if.else, !dbg !64
//CHECK: 
//CHECK: if.then:                                          ; preds = %for.body
//CHECK:   %3 = load double, double* %x.addr, align 8, !dbg !65, !tbaa !46
//CHECK:   %div = fdiv double %3, 1.000000e+01, !dbg !65
//CHECK:   store double %div, double* %x.addr, align 8, !dbg !65, !tbaa !46
//CHECK:   br label %if.end, !dbg !67
//CHECK: 
//CHECK: if.else:                                          ; preds = %for.body
//CHECK:   %conv3 = sitofp i32 %i.0 to double, !dbg !68
//CHECK:   %4 = load double, double* %x.addr, align 8, !dbg !70, !tbaa !46
//CHECK:   %div4 = fdiv double %4, %conv3, !dbg !70
//CHECK:   store double %div4, double* %x.addr, align 8, !dbg !70, !tbaa !46
//CHECK:   br label %if.end
//CHECK: 
//CHECK: if.end:                                           ; preds = %if.else, %if.then
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !21, metadata !DIExpression()), !dbg !37
//CHECK:   br label %for.cond5, !dbg !71
//CHECK: 
//CHECK: for.cond5:                                        ; preds = %for.body9, %if.end
//CHECK:   %j.0 = phi i32 [ 0, %if.end ], [ %inc, %for.body9 ], !dbg !72
//CHECK:   call void @llvm.dbg.value(metadata i32 %j.0, metadata !21, metadata !DIExpression()), !dbg !37
//CHECK:   %cmp6 = icmp slt i32 %j.0, 10, !dbg !74
//CHECK:   br i1 %cmp6, label %for.body9, label %for.cond.cleanup8, !dbg !75
//CHECK: 
//CHECK: for.cond.cleanup8:                                ; preds = %for.cond5
//CHECK:   %inc13 = add nsw i32 %i.0, 1, !dbg !76
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc13, metadata !19, metadata !DIExpression()), !dbg !34
//CHECK:   br label %for.cond, !dbg !77, !llvm.loop !78
//CHECK: 
//CHECK: for.body9:                                        ; preds = %for.cond5
//CHECK:   %conv10 = sitofp i32 %j.0 to double, !dbg !80
//CHECK:   %5 = load double, double* %x.addr, align 8, !dbg !82, !tbaa !46
//CHECK:   %add11 = fadd double %5, %conv10, !dbg !82
//CHECK:   store double %add11, double* %x.addr, align 8, !dbg !82, !tbaa !46
//CHECK:   %inc = add nsw i32 %j.0, 1, !dbg !83
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !21, metadata !DIExpression()), !dbg !37
//CHECK:   br label %for.cond5, !dbg !84, !llvm.loop !85
//CHECK: }
//CHECK: now working with x.addr
//CHECK: validated
//CHECK: analyzed
//CHECK: loads inserted
//CHECK: phis inserted
//CHECK: loads handled
//CHECK: done
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: nounwind uwtable
//CHECK: define dso_local double @foo(double %x, i32 %n, double** %ptr) #0 !dbg !7 !alias.tree !25 {
//CHECK: entry:
//CHECK:   %x.addr = alloca double, align 8
//CHECK:   store double %x, double* %x.addr, align 8, !tbaa !46
//CHECK:   call void @llvm.dbg.declare(metadata double* %x.addr, metadata !16, metadata !DIExpression()), !dbg !42
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !17, metadata !DIExpression()), !dbg !45
//CHECK:   call void @llvm.dbg.value(metadata double** %ptr, metadata !18, metadata !DIExpression()), !dbg !29
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !19, metadata !DIExpression()), !dbg !34
//CHECK:   %load.x.addr = load double, double* %x.addr
//CHECK:   br label %for.cond, !dbg !50
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.cond.cleanup8, %entry
//CHECK:   %phi.for.cond = phi double [ %phi.for.cond5, %for.cond.cleanup8 ], [ %load.x.addr, %entry ]
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc13, %for.cond.cleanup8 ], !dbg !51
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !19, metadata !DIExpression()), !dbg !34
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !52
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !53
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   store double %phi.for.cond, double* %x.addr
//CHECK:   store double* %x.addr, double** %ptr, align 8, !dbg !54, !tbaa !55
//CHECK:   %0 = load double, double* %x.addr, align 8, !dbg !57, !tbaa !46
//CHECK:   ret double %0, !dbg !58
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %conv = sitofp i32 %i.0 to double, !dbg !59
//CHECK:   %mul = fmul double %phi.for.cond, %conv, !dbg !60
//CHECK:   %add = fadd double %mul, 3.000000e+00, !dbg !61
//CHECK:   %cmp1 = icmp sgt i32 %i.0, 10, !dbg !62
//CHECK:   br i1 %cmp1, label %if.then, label %if.else, !dbg !64
//CHECK: 
//CHECK: if.then:                                          ; preds = %for.body
//CHECK:   %div = fdiv double %phi.for.cond, 1.000000e+01, !dbg !65
//CHECK:   br label %if.end, !dbg !67
//CHECK: 
//CHECK: if.else:                                          ; preds = %for.body
//CHECK:   %conv3 = sitofp i32 %i.0 to double, !dbg !68
//CHECK:   %div4 = fdiv double %phi.for.cond, %conv3, !dbg !70
//CHECK:   br label %if.end
//CHECK: 
//CHECK: if.end:                                           ; preds = %if.else, %if.then
//CHECK:   %phi.if.end = phi double [ %div4, %if.else ], [ %div, %if.then ]
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !21, metadata !DIExpression()), !dbg !37
//CHECK:   br label %for.cond5, !dbg !71
//CHECK: 
//CHECK: for.cond5:                                        ; preds = %for.body9, %if.end
//CHECK:   %phi.for.cond5 = phi double [ %add11, %for.body9 ], [ %phi.if.end, %if.end ]
//CHECK:   %j.0 = phi i32 [ 0, %if.end ], [ %inc, %for.body9 ], !dbg !72
//CHECK:   call void @llvm.dbg.value(metadata i32 %j.0, metadata !21, metadata !DIExpression()), !dbg !37
//CHECK:   %cmp6 = icmp slt i32 %j.0, 10, !dbg !74
//CHECK:   br i1 %cmp6, label %for.body9, label %for.cond.cleanup8, !dbg !75
//CHECK: 
//CHECK: for.cond.cleanup8:                                ; preds = %for.cond5
//CHECK:   %inc13 = add nsw i32 %i.0, 1, !dbg !76
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc13, metadata !19, metadata !DIExpression()), !dbg !34
//CHECK:   br label %for.cond, !dbg !77, !llvm.loop !78
//CHECK: 
//CHECK: for.body9:                                        ; preds = %for.cond5
//CHECK:   %conv10 = sitofp i32 %j.0 to double, !dbg !80
//CHECK:   %add11 = fadd double %phi.for.cond5, %conv10, !dbg !82
//CHECK:   %inc = add nsw i32 %j.0, 1, !dbg !83
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !21, metadata !DIExpression()), !dbg !37
//CHECK:   br label %for.cond5, !dbg !84, !llvm.loop !85
//CHECK: }
