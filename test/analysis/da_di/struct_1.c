struct STy { int X; double Y; };

double foo(int N) {
  struct STy S;
  double Res = 0;
  S.Y = S.X = N;
  for (int I = 0; I < S.X; ++I)
    Res += S.Y;
  return Res;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 struct_1.c:7:3
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,,1] | <Res:5:10, 8>:[Fp,,,]
//CHECK:    read only:
//CHECK:     <S:4:14, 16>
//CHECK:    lock:
//CHECK:     <I:7:12, 4> | <S:4:14, 16>
//CHECK:    header access:
//CHECK:     <I:7:12, 4> | <S:4:14, 16>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4> | <Res:5:10, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4> <Res:5:10, 8>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4> <S:4:14, 16>
//CHECK:    direct access (separate):
//CHECK:     <I:7:12, 4> <Res:5:10, 8> <S:4:14, 16>
