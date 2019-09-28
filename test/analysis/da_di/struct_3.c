struct STy { int X; double Y; };

double foo(int N) {
  struct STy S;
  double Res = 0;
  S.Y = S.X = N;
  for (int I = 0; I < S.X; ++I) {
    S.Y += I;
  }
  return S.Y;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 struct_3.c:7:3
//CHECK:    output:
//CHECK:     <S:4:14, 16>
//CHECK:    anti:
//CHECK:     <S:4:14, 16>
//CHECK:    flow:
//CHECK:     <S:4:14, 16>
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,,1]
//CHECK:    lock:
//CHECK:     <I:7:12, 4> | <S:4:14, 16>
//CHECK:    header access:
//CHECK:     <I:7:12, 4> | <S:4:14, 16>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4>
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4> <S:4:14, 16>
//CHECK:    direct access (separate):
//CHECK:     <I:7:12, 4> <S:4:14, 16>
