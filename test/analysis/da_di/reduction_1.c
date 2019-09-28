double foo() {
  double S = 0;
  for (int I = 0; I < 10; ++I)
    S += I;
  return S;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 reduction_1.c:3:3
//CHECK:    induction:
//CHECK:     <I:3:12, 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <S:2:10, 8>:add
//CHECK:    lock:
//CHECK:     <I:3:12, 4>
//CHECK:    header access:
//CHECK:     <I:3:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:3:12, 4> | <S:2:10, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:3:12, 4> <S:2:10, 8>
//CHECK:    lock (separate):
//CHECK:     <I:3:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:3:12, 4> <S:2:10, 8>
