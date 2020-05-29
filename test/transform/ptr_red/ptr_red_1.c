void bar(int *v) { *v = 0; }

int foo(int x, int n) {
  bar(&x);
  for (int i = 0; i < n; ++i) {
    x *= i;
  }
  return x;
}
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: argmemonly nounwind uwtable
//CHECK: define dso_local void @bar(i32* nocapture %v) #0 !dbg !7 !alias.tree !15 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata i32* %v, metadata !14, metadata !DIExpression()), !dbg !20
//CHECK:   store i32 0, i32* %v, align 4, !dbg !24, !tbaa !25
//CHECK:   ret void, !dbg !29
//CHECK: }
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: argmemonly nounwind uwtable
//CHECK: define dso_local void @bar(i32* nocapture %v) #0 !dbg !7 !alias.tree !15 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata i32* %v, metadata !14, metadata !DIExpression()), !dbg !20
//CHECK:   store i32 0, i32* %v, align 4, !dbg !24, !tbaa !25
//CHECK:   ret void, !dbg !29
//CHECK: }
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: argmemonly nounwind uwtable
//CHECK: define dso_local i32 @foo(i32 %x, i32 %n) #0 !dbg !30 !alias.tree !38 {
//CHECK: entry:
//CHECK:   %x.addr = alloca i32, align 4
//CHECK:   store i32 %x, i32* %x.addr, align 4, !tbaa !25
//CHECK:   call void @llvm.dbg.declare(metadata i32* %x.addr, metadata !34, metadata !DIExpression()), !dbg !41
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !35, metadata !DIExpression()), !dbg !44
//CHECK:   call void @bar(i32* %x.addr), !dbg !50
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !36, metadata !DIExpression()), !dbg !47
//CHECK:   br label %for.cond, !dbg !55
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.body, %entry
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.body ], !dbg !56
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !36, metadata !DIExpression()), !dbg !47
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !58
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !59
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   %0 = load i32, i32* %x.addr, align 4, !dbg !60, !tbaa !25
//CHECK:   ret i32 %0, !dbg !61
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %1 = load i32, i32* %x.addr, align 4, !dbg !62, !tbaa !25
//CHECK:   %mul = mul nsw i32 %1, %i.0, !dbg !62
//CHECK:   store i32 %mul, i32* %x.addr, align 4, !dbg !62, !tbaa !25
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !64
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !36, metadata !DIExpression()), !dbg !47
//CHECK:   br label %for.cond, !dbg !65, !llvm.loop !66
//CHECK: }
//CHECK: now working with x.addr
//CHECK: validated
//CHECK: analyzed
//CHECK: loads inserted
//CHECK: phis inserted
//CHECK: loads handled
//CHECK: done
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: argmemonly nounwind uwtable
//CHECK: define dso_local i32 @foo(i32 %x, i32 %n) #0 !dbg !30 !alias.tree !38 {
//CHECK: entry:
//CHECK:   %x.addr = alloca i32, align 4
//CHECK:   store i32 %x, i32* %x.addr, align 4, !tbaa !25
//CHECK:   call void @llvm.dbg.declare(metadata i32* %x.addr, metadata !34, metadata !DIExpression()), !dbg !41
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !35, metadata !DIExpression()), !dbg !44
//CHECK:   call void @bar(i32* %x.addr), !dbg !50
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !36, metadata !DIExpression()), !dbg !47
//CHECK:   %load.x.addr = load i32, i32* %x.addr
//CHECK:   br label %for.cond, !dbg !55
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.body, %entry
//CHECK:   %phi.for.cond = phi i32 [ %mul, %for.body ], [ %load.x.addr, %entry ]
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.body ], !dbg !56
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !36, metadata !DIExpression()), !dbg !47
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !58
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !59
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   store i32 %phi.for.cond, i32* %x.addr
//CHECK:   %0 = load i32, i32* %x.addr, align 4, !dbg !60, !tbaa !25
//CHECK:   ret i32 %0, !dbg !61
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %mul = mul nsw i32 %phi.for.cond, %i.0, !dbg !62
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !64
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !36, metadata !DIExpression()), !dbg !47
//CHECK:   br label %for.cond, !dbg !65, !llvm.loop !66
//CHECK: }
