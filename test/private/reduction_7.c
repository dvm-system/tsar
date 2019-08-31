double foo(double S, double Q) {
  for (int J = 0; J < 10; ++J) {
    for (int I = 0; I < 10; ++I) {
      S += I;
    }
    S = Q;
  }
  return S + Q;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 reduction_7.c:2:3
//CHECK:    private:
//CHECK:     <I:3:14, 4>
//CHECK:    output:
//CHECK:     <S:1:19, 8>
//CHECK:    anti:
//CHECK:     <S:1:19, 8>
//CHECK:    flow:
//CHECK:     <S:1:19, 8>
//CHECK:    induction:
//CHECK:     <J:2:12, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <Q:1:29, 8>
//CHECK:    lock:
//CHECK:     <J:2:12, 4>
//CHECK:    header access:
//CHECK:     <J:2:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:3:14, 4> | <J:2:12, 4> | <Q:1:29, 8> | <S:1:19, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:3:14, 4> <J:2:12, 4> <Q:1:29, 8> <S:1:19, 8>
//CHECK:    lock (separate):
//CHECK:     <J:2:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:3:14, 4> <J:2:12, 4> <Q:1:29, 8> <S:1:19, 8>
//CHECK:   loop at depth 2 reduction_7.c:3:5
//CHECK:     output:
//CHECK:      <S:1:19, 8>
//CHECK:     anti:
//CHECK:      <S:1:19, 8>
//CHECK:     flow:
//CHECK:      <S:1:19, 8>
//CHECK:     induction:
//CHECK:      <I:3:14, 4>:[Int,0,10,1]
//CHECK:     lock:
//CHECK:      <I:3:14, 4>
//CHECK:     header access:
//CHECK:      <I:3:14, 4>
//CHECK:     explicit access:
//CHECK:      <I:3:14, 4> | <S:1:19, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:3:14, 4> <S:1:19, 8>
//CHECK:     lock (separate):
//CHECK:      <I:3:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:3:14, 4> <S:1:19, 8>
