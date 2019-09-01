struct STy { int X; double Y; };

double foo(struct STy S) {
  double Res = 0;
  for (int I = 0; I < S.X; ++I)
    Res += S.Y;
  return Res;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 struct_5.c:5:3
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,0,,1]
//CHECK:    reduction:
//CHECK:     <Res:4:10, 8>:add
//CHECK:    read only:
//CHECK:     <S:{5:25|3:23}, 16>
//CHECK:    lock:
//CHECK:     <I:5:12, 4> | <S:{5:25|3:23}, 16>
//CHECK:    header access:
//CHECK:     <I:5:12, 4> | <S:{5:25|3:23}, 16>
//CHECK:    explicit access:
//CHECK:     <I:5:12, 4> | <Res:4:10, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <Res:4:10, 8>
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4> <S:{5:25|3:23}, 16>
//CHECK:    direct access (separate):
//CHECK:     <I:5:12, 4> <Res:4:10, 8> <S:{5:25|3:23}, 16>
