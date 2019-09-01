struct STy { int X; double Y; };

double foo(int N, struct STy S) {
  for (int I = 0; I < N; ++I) {
    S.Y += I;
    S.X *= I;
  }
  return S.Y + S.X;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 struct_6.c:4:3
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,0,,1]
//CHECK:    reduction:
//CHECK:     <S:4:14, 16>
//CHECK:    read only:
//CHECK:     <N:3:16, 4>
//CHECK:    lock:
//CHECK:     <I:4:12, 4> | <N:3:16, 4>
//CHECK:    header access:
//CHECK:     <I:4:12, 4> | <N:3:16, 4>
//CHECK:    explicit access:
//CHECK:     <I:4:12, 4> | <N:3:16, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:4:12, 4> <N:3:16, 4>
//CHECK:    lock (separate):
//CHECK:     <I:4:12, 4> <N:3:16, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:4:12, 4> <N:3:16, 4> <S:3:30, 16>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 struct_6.c:4:3
//SAFE:    output:
//SAFE:     <S:3:30, 16>
//SAFE:    anti:
//SAFE:     <S:3:30, 16>
//SAFE:    flow:
//SAFE:     <S:3:30, 16>
//SAFE:    induction:
//SAFE:     <I:4:12, 4>:[Int,0,,1]
//SAFE:    read only:
//SAFE:     <N:3:16, 4>
//SAFE:    lock:
//SAFE:     <I:4:12, 4> | <N:3:16, 4>
//SAFE:    header access:
//SAFE:     <I:4:12, 4> | <N:3:16, 4>
//SAFE:    explicit access:
//SAFE:     <I:4:12, 4> | <N:3:16, 4>
//SAFE:    explicit access (separate):
//SAFE:     <I:4:12, 4> <N:3:16, 4>
//SAFE:    lock (separate):
//SAFE:     <I:4:12, 4> <N:3:16, 4>
//SAFE:    direct access (separate):
//SAFE:     <I:4:12, 4> <N:3:16, 4> <S:3:30, 16>
