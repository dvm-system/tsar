struct STy { int X; double Y; };

double foo(int N) {
  struct STy S;
  S.Y = S.X = N;
  for (int I = 0; I < N; ++I) {
    S.Y += I;
    S.X *= I;
  }
  return S.Y + S.X;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 struct_4.c:6:3
//CHECK:    induction:
//CHECK:     <I:6:12, 4>:[Int,0,,1]
//CHECK:    reduction:
//CHECK:     <S:4:14, 16>
//CHECK:    read only:
//CHECK:     <N:3:16, 4>
//CHECK:    lock:
//CHECK:     <I:6:12, 4> | <N:3:16, 4>
//CHECK:    header access:
//CHECK:     <I:6:12, 4> | <N:3:16, 4>
//CHECK:    explicit access:
//CHECK:     <I:6:12, 4> | <N:3:16, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:6:12, 4> <N:3:16, 4>
//CHECK:    lock (separate):
//CHECK:     <I:6:12, 4> <N:3:16, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:6:12, 4> <N:3:16, 4> <S:4:14, 16>
