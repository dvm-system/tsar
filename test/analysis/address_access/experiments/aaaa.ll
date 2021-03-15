Whole function: 
  %a.addr = alloca i32*, align 8
  %b.addr = alloca i32*, align 8
  %ptra.addr = alloca i32**, align 8
  %ptr = alloca i32**, align 8
  store i32* %a, i32** %a.addr, align 8, !tbaa !24
  call void @llvm.dbg.declare(metadata i32** %a.addr, metadata !20, metadata !DIExpression()), !dbg !28
  store i32* %b, i32** %b.addr, align 8, !tbaa !24
  call void @llvm.dbg.declare(metadata i32** %b.addr, metadata !21, metadata !DIExpression()), !dbg !29
  store i32** %ptra, i32*** %ptra.addr, align 8, !tbaa !24
  call void @llvm.dbg.declare(metadata i32*** %ptra.addr, metadata !22, metadata !DIExpression()), !dbg !30
  %0 = bitcast i32*** %ptr to i8*, !dbg !31
  call void @llvm.lifetime.start.p0i8(i64 8, i8* %0) #4, !dbg !31
  call void @llvm.dbg.declare(metadata i32*** %ptr, metadata !23, metadata !DIExpression()), !dbg !32
  br label %1, !dbg !33
  %call = call i8* @malloc(i64 8) #5, !dbg !33
  br label %2, !dbg !34
  %3 = bitcast i8* %call to i32**, !dbg !34
  store i32** %3, i32*** %ptr, align 8, !dbg !35, !tbaa !24
  %4 = load i32**, i32*** %ptr, align 8, !dbg !36, !tbaa !24
  store i32** %4, i32*** @glob, align 8, !dbg !37, !tbaa !24
  %5 = load i32*, i32** %a.addr, align 8, !dbg !38, !tbaa !24  ; %5 mustalias %a ?
  %6 = load i32**, i32*** %ptr, align 8, !dbg !39, !tbaa !24
  store i32* %5, i32** %6, align 8, !dbg !40, !tbaa !24
  %7 = load i32*, i32** %b.addr, align 8, !dbg !41, !tbaa !24
  %8 = load i32**, i32*** %ptra.addr, align 8, !dbg !42, !tbaa !24
  store i32* %7, i32** %8, align 8, !dbg !43, !tbaa !24
  %9 = bitcast i32*** %ptr to i8*, !dbg !44
  call void @llvm.lifetime.end.p0i8(i64 8, i8* %9) #4, !dbg !44
  ret void, !dbg !44