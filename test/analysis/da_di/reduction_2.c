double foo() {
  double S = 0;
  for (int J = 0; J < 10; ++J)
    for (int I = 0; I < 10; ++I)
      S += I;
  return S;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 reduction_2.c:3:3
//CHECK:    private:
//CHECK:     <I:4[4:5], 4>
//CHECK:    induction:
//CHECK:     <J:3[3:3], 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <S:2, 8>:add
//CHECK:    lock:
//CHECK:     <J:3[3:3], 4>
//CHECK:    header access:
//CHECK:     <J:3[3:3], 4>
//CHECK:    explicit access:
//CHECK:     <I:4[4:5], 4> | <J:3[3:3], 4> | <S:2, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:4[4:5], 4> <J:3[3:3], 4> <S:2, 8>
//CHECK:    lock (separate):
//CHECK:     <J:3[3:3], 4>
//CHECK:    direct access (separate):
//CHECK:     <I:4[4:5], 4> <J:3[3:3], 4> <S:2, 8>
//CHECK:   loop at depth 2 reduction_2.c:4:5
//CHECK:     induction:
//CHECK:      <I:4[4:5], 4>:[Int,0,10,1]
//CHECK:     reduction:
//CHECK:      <S:2, 8>:add
//CHECK:     lock:
//CHECK:      <I:4[4:5], 4>
//CHECK:     header access:
//CHECK:      <I:4[4:5], 4>
//CHECK:     explicit access:
//CHECK:      <I:4[4:5], 4> | <S:2, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:4[4:5], 4> <S:2, 8>
//CHECK:     lock (separate):
//CHECK:      <I:4[4:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <I:4[4:5], 4> <S:2, 8>
