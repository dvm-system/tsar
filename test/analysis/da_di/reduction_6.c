double foo(double S, double Q) {
  for (int J = 0; J < 10; ++J) {
    for (int I = 0; I < 10; ++I) {
      S += I;
      Q *= I;
      S = Q; //analysis capability degradation since LLVM 11
    }
    S = Q;
  }
  return S + Q;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 reduction_6.c:2:3
//CHECK:    private:
//CHECK:     <I:3[3:5], 4>
//CHECK:    induction:
//CHECK:     <J:2[2:3], 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <Q:1, 8>:mult | <S:1, 8>:mult
//CHECK:    lock:
//CHECK:     <J:2[2:3], 4>
//CHECK:    header access:
//CHECK:     <J:2[2:3], 4>
//CHECK:    explicit access:
//CHECK:     <I:3[3:5], 4> | <J:2[2:3], 4> | <Q:1, 8> | <S:1, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:3[3:5], 4> <J:2[2:3], 4> <Q:1, 8> <S:1, 8>
//CHECK:    lock (separate):
//CHECK:     <J:2[2:3], 4>
//CHECK:    direct access (separate):
//CHECK:     <I:3[3:5], 4> <J:2[2:3], 4> <Q:1, 8> <S:1, 8>
//CHECK:   loop at depth 2 reduction_6.c:3:5
//CHECK:     induction:
//CHECK:      <I:3[3:5], 4>:[Int,0,10,1]
//CHECK:     reduction:
//CHECK:      <Q:1, 8>:mult | <S:1, 8>:mult
//CHECK:     lock:
//CHECK:      <I:3[3:5], 4>
//CHECK:     header access:
//CHECK:      <I:3[3:5], 4>
//CHECK:     explicit access:
//CHECK:      <I:3[3:5], 4> | <Q:1, 8> | <S:1, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:3[3:5], 4> <Q:1, 8> <S:1, 8>
//CHECK:     lock (separate):
//CHECK:      <I:3[3:5], 4>
//CHECK:     direct access (separate):
//CHECK:      <I:3[3:5], 4> <Q:1, 8> <S:1, 8>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 reduction_6.c:2:3
//SAFE:    private:
//SAFE:     <I:3[3:5], 4>
//SAFE:    output:
//SAFE:     <Q:1, 8> | <S:1, 8>
//SAFE:    anti:
//SAFE:     <Q:1, 8> | <S:1, 8>
//SAFE:    flow:
//SAFE:     <Q:1, 8> | <S:1, 8>
//SAFE:    induction:
//SAFE:     <J:2[2:3], 4>:[Int,0,10,1]
//SAFE:    lock:
//SAFE:     <J:2[2:3], 4>
//SAFE:    header access:
//SAFE:     <J:2[2:3], 4>
//SAFE:    explicit access:
//SAFE:     <I:3[3:5], 4> | <J:2[2:3], 4> | <Q:1, 8> | <S:1, 8>
//SAFE:    explicit access (separate):
//SAFE:     <I:3[3:5], 4> <J:2[2:3], 4> <Q:1, 8> <S:1, 8>
//SAFE:    lock (separate):
//SAFE:     <J:2[2:3], 4>
//SAFE:    direct access (separate):
//SAFE:     <I:3[3:5], 4> <J:2[2:3], 4> <Q:1, 8> <S:1, 8>
//SAFE:   loop at depth 2 reduction_6.c:3:5
//SAFE:     output:
//SAFE:      <S:1, 8>
//SAFE:     anti:
//SAFE:      <S:1, 8>
//SAFE:     flow:
//SAFE:      <S:1, 8>
//SAFE:     induction:
//SAFE:      <I:3[3:5], 4>:[Int,0,10,1]
//SAFE:     reduction:
//SAFE:      <Q:1, 8>:mult
//SAFE:     lock:
//SAFE:      <I:3[3:5], 4>
//SAFE:     header access:
//SAFE:      <I:3[3:5], 4>
//SAFE:     explicit access:
//SAFE:      <I:3[3:5], 4> | <Q:1, 8> | <S:1, 8>
//SAFE:     explicit access (separate):
//SAFE:      <I:3[3:5], 4> <Q:1, 8> <S:1, 8>
//SAFE:     lock (separate):
//SAFE:      <I:3[3:5], 4>
//SAFE:     direct access (separate):
//SAFE:      <I:3[3:5], 4> <Q:1, 8> <S:1, 8>
