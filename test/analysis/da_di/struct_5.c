struct STy { int X; double Y; };

double foo(struct STy S) {
  double Res = 0;
  for (int I = 0; I < S.X; ++I)
    Res += S.Y;
  return Res;
}
//WINDOWS: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//WINDOWS:  loop at depth 1 struct_5.c:5:3
//WINDOWS:    induction:
//WINDOWS:     <I:5[5:3], 4>:[Int,0,,1]
//WINDOWS:    reduction:
//WINDOWS:     <Res:4, 8>:add
//WINDOWS:    read only:
//WINDOWS:     <S:{5:25|3:23}, 16>
//WINDOWS:    lock:
//WINDOWS:     <I:5[5:3], 4> | <S:{5:25|3:23}, 16>
//WINDOWS:    header access:
//WINDOWS:     <I:5[5:3], 4> | <S:{5:25|3:23}, 16>
//WINDOWS:    explicit access:
//WINDOWS:     <I:5[5:3], 4> | <Res:4, 8>
//WINDOWS:    explicit access (separate):
//WINDOWS:     <I:5[5:3], 4> <Res:4, 8>
//WINDOWS:    lock (separate):
//WINDOWS:     <I:5[5:3], 4> <S:{5:25|3:23}, 16>
//WINDOWS:    direct access (separate):
//WINDOWS:     <I:5[5:3], 4> <Res:4, 8> <S:{5:25|3:23}, 16>
//NOT_WINDOWS: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//NOT_WINDOWS:  loop at depth 1 struct_5.c:5:3
//NOT_WINDOWS:    induction:
//NOT_WINDOWS:     <I:5[5:3], 4>:[Int,0,,1] | <Res:4, 8>:[Fp,,,]
//NOT_WINDOWS:    read only:
//NOT_WINDOWS:     <S:3, 16>
//NOT_WINDOWS:    lock:
//NOT_WINDOWS:     <I:5[5:3], 4> | <S:3, 16>
//NOT_WINDOWS:    header access:
//NOT_WINDOWS:     <I:5[5:3], 4> | <S:3, 16>
//NOT_WINDOWS:    explicit access:
//NOT_WINDOWS:     <I:5[5:3], 4> | <Res:4, 8>
//NOT_WINDOWS:    explicit access (separate):
//NOT_WINDOWS:     <I:5[5:3], 4> <Res:4, 8>
//NOT_WINDOWS:    lock (separate):
//NOT_WINDOWS:     <I:5[5:3], 4> <S:3, 16>
//NOT_WINDOWS:    direct access (separate):
//NOT_WINDOWS:     <I:5[5:3], 4> <Res:4, 8> <S:3, 16>
