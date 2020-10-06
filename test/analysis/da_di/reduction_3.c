double foo(double S) {
  for (int J = 0; J < 10; ++J) {
    for (int I = 0; I < 10; ++I)
      S *= I * J;
    for (int I = 0; I < 10; ++I)
      S *= I;
  }
  return S;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 reduction_3.c:2:3
//CHECK:    private:
//CHECK:     <I:3[3:5], 4> | <I:5[5:5], 4>
//CHECK:    induction:
//CHECK:     <J:2[2:3], 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <S:1, 8>:mult
//CHECK:    lock:
//CHECK:     <J:2[2:3], 4>
//CHECK:    header access:
//CHECK:     <J:2[2:3], 4>
//CHECK:    explicit access:
//CHECK:     <I:3[3:5], 4> | <I:5[5:5], 4> | <J:2[2:3], 4> | <S:1, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:3[3:5], 4> <I:5[5:5], 4> <J:2[2:3], 4> <S:1, 8>
//CHECK:    lock (separate):
//CHECK:     <J:2[2:3], 4>
//CHECK:    direct access (separate):
//CHECK:     <I:3[3:5], 4> <I:5[5:5], 4> <J:2[2:3], 4> <S:1, 8>
//CHECK:   loop at depth 2 reduction_3.c:5:5
//CHECK:     induction:
//CHECK:      <I:5[5:5], 4>:[Int,0,10,1]
//CHECK:     reduction:
//CHECK:      <S:1, 8>:mult
//CHECK:     lock:
//CHECK:      <I:5[5:5], 4>
//CHECK:     header access:
//CHECK:      <I:5[5:5], 4>
//CHECK:     explicit access:
//CHECK:      <I:5[5:5], 4> | <S:1, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:5[5:5], 4> <S:1, 8>
//CHECK:     lock (separate):
//CHECK:      <I:5[5:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <I:5[5:5], 4> <S:1, 8>
//CHECK:   loop at depth 2 reduction_3.c:3:5
//CHECK:     induction:
//CHECK:      <I:3[3:5], 4>:[Int,0,10,1]
//CHECK:     reduction:
//CHECK:      <S:1, 8>:mult
//CHECK:     read only:
//CHECK:      <J:2[2:3], 4>
//CHECK:     lock:
//CHECK:      <I:3[3:5], 4>
//CHECK:     header access:
//CHECK:      <I:3[3:5], 4>
//CHECK:     explicit access:
//CHECK:      <I:3[3:5], 4> | <J:2[2:3], 4> | <S:1, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:3[3:5], 4> <J:2[2:3], 4> <S:1, 8>
//CHECK:     lock (separate):
//CHECK:      <I:3[3:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <I:3[3:5], 4> <J:2[2:3], 4> <S:1, 8>
