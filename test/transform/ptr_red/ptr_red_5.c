int foo(int *x, int n) {
  for (int i = 0; i < n; ++i) {
    *x *= i;
    if (i == 5) {
      x = 3;
    }
  }
  return x;
}
//CHECK: ptr_red_5.c:5:9: warning: incompatible integer to pointer conversion assigning to 'int *' from 'int' [-Wint-conversion]
//CHECK:       x = 3;
//CHECK:         ^ ~
//CHECK: ptr_red_5.c:8:10: warning: incompatible pointer to integer conversion returning 'int *' from a function with result type 'int'; dereference with * [-Wint-conversion]
//CHECK:   return x;
//CHECK:          ^
//CHECK:          *
//CHECK: *** IR Dump Before Pointer Reduction Pass ***
//CHECK: ; Function Attrs: argmemonly nounwind uwtable
//CHECK: define dso_local i32 @foo(i32* nocapture %x, i32 %n) #0 !dbg !7 !alias.tree !18 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata i32* %x, metadata !14, metadata !DIExpression()), !dbg !24
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !15, metadata !DIExpression()), !dbg !21
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !16, metadata !DIExpression()), !dbg !32
//CHECK:   br label %for.cond, !dbg !39
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.inc, %entry
//CHECK:   %x.addr.0 = phi i32* [ %x, %entry ], [ %x.addr.1, %for.inc ]
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.inc ], !dbg !40
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !16, metadata !DIExpression()), !dbg !32
//CHECK:   call void @llvm.dbg.value(metadata i32* %x.addr.0, metadata !14, metadata !DIExpression()), !dbg !24
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !41
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !42
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   %0 = ptrtoint i32* %x.addr.0 to i32, !dbg !38
//CHECK:   ret i32 %0, !dbg !43
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %1 = load i32, i32* %x.addr.0, align 4, !dbg !44, !tbaa !45
//CHECK:   %mul = mul nsw i32 %1, %i.0, !dbg !44
//CHECK:   store i32 %mul, i32* %x.addr.0, align 4, !dbg !44, !tbaa !45
//CHECK:   %cmp1 = icmp eq i32 %i.0, 5, !dbg !49
//CHECK:   br i1 %cmp1, label %if.then, label %for.inc, !dbg !51
//CHECK: 
//CHECK: if.then:                                          ; preds = %for.body
//CHECK:   call void @llvm.dbg.value(metadata i32* inttoptr (i64 3 to i32*), metadata !14, metadata !DIExpression()), !dbg !24
//CHECK:   br label %for.inc, !dbg !52
//CHECK: 
//CHECK: for.inc:                                          ; preds = %for.body, %if.then
//CHECK:   %x.addr.1 = phi i32* [ inttoptr (i64 3 to i32*), %if.then ], [ %x.addr.0, %for.body ]
//CHECK:   call void @llvm.dbg.value(metadata i32* %x.addr.1, metadata !14, metadata !DIExpression()), !dbg !24
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !54
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !16, metadata !DIExpression()), !dbg !32
//CHECK:   br label %for.cond, !dbg !55, !llvm.loop !56
//CHECK: }
//CHECK: now working with x.addr.0
//CHECK: validated
//CHECK: 1 found
//CHECK: *** IR Dump After Pointer Reduction Pass ***
//CHECK: ; Function Attrs: argmemonly nounwind uwtable
//CHECK: define dso_local i32 @foo(i32* nocapture %x, i32 %n) #0 !dbg !7 !alias.tree !18 {
//CHECK: entry:
//CHECK:   call void @llvm.dbg.value(metadata i32* %x, metadata !14, metadata !DIExpression()), !dbg !24
//CHECK:   call void @llvm.dbg.value(metadata i32 %n, metadata !15, metadata !DIExpression()), !dbg !21
//CHECK:   call void @llvm.dbg.value(metadata i32 0, metadata !16, metadata !DIExpression()), !dbg !32
//CHECK:   br label %for.cond, !dbg !39
//CHECK: 
//CHECK: for.cond:                                         ; preds = %for.inc, %entry
//CHECK:   %x.addr.0 = phi i32* [ %x, %entry ], [ %x.addr.1, %for.inc ]
//CHECK:   %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.inc ], !dbg !40
//CHECK:   call void @llvm.dbg.value(metadata i32 %i.0, metadata !16, metadata !DIExpression()), !dbg !32
//CHECK:   call void @llvm.dbg.value(metadata i32* %x.addr.0, metadata !14, metadata !DIExpression()), !dbg !24
//CHECK:   %cmp = icmp slt i32 %i.0, %n, !dbg !41
//CHECK:   br i1 %cmp, label %for.body, label %for.cond.cleanup, !dbg !42
//CHECK: 
//CHECK: for.cond.cleanup:                                 ; preds = %for.cond
//CHECK:   %0 = ptrtoint i32* %x.addr.0 to i32, !dbg !38
//CHECK:   ret i32 %0, !dbg !43
//CHECK: 
//CHECK: for.body:                                         ; preds = %for.cond
//CHECK:   %1 = load i32, i32* %x.addr.0, align 4, !dbg !44, !tbaa !45
//CHECK:   %mul = mul nsw i32 %1, %i.0, !dbg !44
//CHECK:   store i32 %mul, i32* %x.addr.0, align 4, !dbg !44, !tbaa !45
//CHECK:   %cmp1 = icmp eq i32 %i.0, 5, !dbg !49
//CHECK:   br i1 %cmp1, label %if.then, label %for.inc, !dbg !51
//CHECK: 
//CHECK: if.then:                                          ; preds = %for.body
//CHECK:   call void @llvm.dbg.value(metadata i32* inttoptr (i64 3 to i32*), metadata !14, metadata !DIExpression()), !dbg !24
//CHECK:   br label %for.inc, !dbg !52
//CHECK: 
//CHECK: for.inc:                                          ; preds = %for.body, %if.then
//CHECK:   %x.addr.1 = phi i32* [ inttoptr (i64 3 to i32*), %if.then ], [ %x.addr.0, %for.body ]
//CHECK:   call void @llvm.dbg.value(metadata i32* %x.addr.1, metadata !14, metadata !DIExpression()), !dbg !24
//CHECK:   %inc = add nsw i32 %i.0, 1, !dbg !54
//CHECK:   call void @llvm.dbg.value(metadata i32 %inc, metadata !16, metadata !DIExpression()), !dbg !32
//CHECK:   br label %for.cond, !dbg !55, !llvm.loop !56
//CHECK: }
//CHECK: 2 warnings generated.
