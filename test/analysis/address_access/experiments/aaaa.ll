Whole function: 
  %a.addr = alloca i32*, align 8
  %b.addr = alloca i32*, align 8
  %ptra.addr = alloca i32**, align 8
  %ptr = alloca i32**, align 8
  %c = alloca i32*, align 8
  store i32* %a, i32** %a.addr, align 8, !tbaa !25
  call void @llvm.dbg.declare(metadata i32** %a.addr, metadata !20, metadata !DIExpression()), !dbg !29
  store i32* %b, i32** %b.addr, align 8, !tbaa !25
  call void @llvm.dbg.declare(metadata i32** %b.addr, metadata !21, metadata !DIExpression()), !dbg !30
  store i32** %ptra, i32*** %ptra.addr, align 8, !tbaa !25
  call void @llvm.dbg.declare(metadata i32*** %ptra.addr, metadata !22, metadata !DIExpression()), !dbg !31
  %0 = bitcast i32*** %ptr to i8*, !dbg !32
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %0) #4, !dbg !32
  call void @llvm.dbg.declare(metadata i32*** %ptr, metadata !23, metadata !DIExpression()), !dbg !33
  br label %1, !dbg !34
  %call = call i8* @malloc(i64 8) #5, !dbg !34
  br label %2, !dbg !35
  %3 = bitcast i8* %call to i32**, !dbg !35
  store i32** %3, i32*** %ptr, align 8, !dbg !36, !tbaa !25
  %4 = load i32**, i32*** %ptr, align 8, !dbg !37, !tbaa !25
  store i32** %4, i32*** @glob, align 8, !dbg !38, !tbaa !25
  %5 = bitcast i32** %c to i8*, !dbg !39
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %5) #4, !dbg !39
  call void @llvm.dbg.declare(metadata i32** %c, metadata !24, metadata !DIExpression()), !dbg !40
  %6 = load i32*, i32** %a.addr, align 8, !dbg !41, !tbaa !25
  %arrayidx = getelementptr inbounds i32, i32* %6, i64 3, !dbg !41
  store i32* %arrayidx, i32** %c, align 8, !dbg !40, !tbaa !25
  %7 = load i32*, i32** %c, align 8, !dbg !42, !tbaa !25
  %8 = load i32**, i32*** %ptr, align 8, !dbg !43, !tbaa !25
  store i32* %7, i32** %8, align 8, !dbg !44, !tbaa !25
  %9 = bitcast i32** %c to i8*, !dbg !45
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %9) #4, !dbg !45
  %10 = bitcast i32*** %ptr to i8*, !dbg !45
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %10) #4, !dbg !45
  ret void, !dbg !45


  