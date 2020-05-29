int *x;

void bar(int *v) { x = v; }

void g() { *x = 0; }

int foo(int x, int n) {
  bar(&x);
  for (int i = 0; i < n; ++i) {
    x *= i;
    if (i == 10) {
      g();
    }
  }
  return x;
}
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: nounwind uwtable
//CHECK: define dso_local void @bar(i32* %v) #0 !dbg !13 !alias.tree !18 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata i32* %v, metadata !17, metadata !DIExpression()), !dbg !21
//CHECK:   store i32* %v, i32** @x, align 8, !dbg !29, !tbaa !30
//CHECK:   ret void, !dbg !34
//CHECK: }
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: nounwind uwtable
//CHECK: define dso_local void @bar(i32* %v) #0 !dbg !13 !alias.tree !18 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata i32* %v, metadata !17, metadata !DIExpression()), !dbg !21
//CHECK:   store i32* %v, i32** @x, align 8, !dbg !29, !tbaa !30
//CHECK:   ret void, !dbg !34
//CHECK: }
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: norecurse nounwind uwtable
//CHECK: define dso_local void @g() #2 !dbg !35 !alias.tree !38 {
//CHECK: entry:
//CHECK:   %0 = load i32*, i32** @x, align 8, !dbg !41, !tbaa !30
//CHECK:   store i32 0, i32* %0, align 4, !dbg !42, !tbaa !43
//CHECK:   ret void, !dbg !45
//CHECK: }
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: norecurse nounwind uwtable
//CHECK: define dso_local void @g() #2 !dbg !35 !alias.tree !38 {
//CHECK: entry:
//CHECK:   %0 = load i32*, i32** @x, align 8, !dbg !41, !tbaa !30
//CHECK:   store i32 0, i32* %0, align 4, !dbg !42, !tbaa !43
//CHECK:   ret void, !dbg !45
//CHECK: }
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: nounwind uwtable
//CHECK: define dso_local i32 @foo(i32 %x, i32 %n) #0 !dbg !46 !alias.tree !54 {
//CHECK: entry:
//CHECK:   %x.addr = alloca i32, align 4
//CHECK:   store i32 %x, i32* %x.addr, align 4, !tbaa !43
//CHECK:   call void @llvm.dbg.declare(metadata i32* %x.addr, metadata !50, metadata !DIExpression()), !dbg !62
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !51, metadata !DIExpression()), !dbg !68
//CHECK:   call void @bar(i32* %x.addr), !dbg !65
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !52, metadata !DIExpression()), !dbg !57
//CHECK:   br label %for.cond, !dbg !80
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.inc, %entry
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.inc ], !dbg !81
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !52, metadata !DIExpression()), !dbg !57
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !82
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !83
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   %0 = load i32, i32* %x.addr, align 4, !dbg !84, !tbaa !43
//CHECK:   ret i32 %0, !dbg !85
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %1 = load i32, i32* %x.addr, align 4, !dbg !86, !tbaa !43
//CHECK:   %mul = mul nsw i32 %1, %i.0, !dbg !86
//CHECK:   store i32 %mul, i32* %x.addr, align 4, !dbg !86, !tbaa !43
//CHECK:   %cmp1 = icmp eq i32 %i.0, 10, !dbg !87
//CHECK:   br i1 %cmp1, label %if.then, label %for.inc, !dbg !88
//CHECK: 
//CHECK: if.then:                                          ; preds = %for.body
//CHECK:   call void @g(), !dbg !71
//CHECK:   br label %for.inc, !dbg !89
//CHECK: 
//CHECK: for.inc:                                          ; preds = %for.body, %if.then
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !90
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !52, metadata !DIExpression()), !dbg !57
//CHECK:   br label %for.cond, !dbg !91, !llvm.loop !92
//CHECK: }
//CHECK: now working with x.addr
//CHECK: validated
//CHECK: 3 found
//CHECK: if.then
//CHECK: foo
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: nounwind uwtable
//CHECK: define dso_local i32 @foo(i32 %x, i32 %n) #0 !dbg !46 !alias.tree !54 {
//CHECK: entry:
//CHECK:   %x.addr = alloca i32, align 4
//CHECK:   store i32 %x, i32* %x.addr, align 4, !tbaa !43
//CHECK:   call void @llvm.dbg.declare(metadata i32* %x.addr, metadata !50, metadata !DIExpression()), !dbg !62
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !51, metadata !DIExpression()), !dbg !68
//CHECK:   call void @bar(i32* %x.addr), !dbg !65
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !52, metadata !DIExpression()), !dbg !57
//CHECK:   br label %for.cond, !dbg !80
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.inc, %entry
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.inc ], !dbg !81
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !52, metadata !DIExpression()), !dbg !57
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !82
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !83
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   %0 = load i32, i32* %x.addr, align 4, !dbg !84, !tbaa !43
//CHECK:   ret i32 %0, !dbg !85
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %1 = load i32, i32* %x.addr, align 4, !dbg !86, !tbaa !43
//CHECK:   %mul = mul nsw i32 %1, %i.0, !dbg !86
//CHECK:   store i32 %mul, i32* %x.addr, align 4, !dbg !86, !tbaa !43
//CHECK:   %cmp1 = icmp eq i32 %i.0, 10, !dbg !87
//CHECK:   br i1 %cmp1, label %if.then, label %for.inc, !dbg !88
//CHECK: 
//CHECK: if.then:                                          ; preds = %for.body
//CHECK:   call void @g(), !dbg !71
//CHECK:   br label %for.inc, !dbg !89
//CHECK: 
//CHECK: for.inc:                                          ; preds = %for.body, %if.then
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !90
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !52, metadata !DIExpression()), !dbg !57
//CHECK:   br label %for.cond, !dbg !91, !llvm.loop !92
//CHECK: }
