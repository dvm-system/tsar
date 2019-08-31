double A[10][10][10];

double foo() {
  double S = 0;
  double S1 = 0;
  for (int K = 0; K < 10; ++K) {
    for (int J = 0; J < 10; ++J)
      for (int I = 0; I < 10; ++I)
        if (S > A[I][J][K])
          S = A[I][J][K];
     S1 += S;
  }
  return S + S1;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 reduction_4.c:6:3
//CHECK:    private:
//CHECK:     <I:8:16, 4> | <J:7:14, 4>
//CHECK:    output:
//CHECK:     <S:4:10, 8>
//CHECK:    anti:
//CHECK:     <S:4:10, 8>
//CHECK:    flow:
//CHECK:     <S:4:10, 8>
//CHECK:    induction:
//CHECK:     <K:6:12, 4>:[Int,0,10,1]
//CHECK:    reduction:
//CHECK:     <S1:5:10, 8>:add
//CHECK:    read only:
//CHECK:     <A, 8000>
//CHECK:    lock:
//CHECK:     <K:6:12, 4>
//CHECK:    header access:
//CHECK:     <K:6:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:8:16, 4> | <J:7:14, 4> | <K:6:12, 4> | <S1:5:10, 8> | <S:4:10, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:8:16, 4> <J:7:14, 4> <K:6:12, 4> <S1:5:10, 8> <S:4:10, 8>
//CHECK:    lock (separate):
//CHECK:     <K:6:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <A, 8000> <I:8:16, 4> <J:7:14, 4> <K:6:12, 4> <S1:5:10, 8> <S:4:10, 8>
//CHECK:   loop at depth 2 reduction_4.c:7:5
//CHECK:     private:
//CHECK:      <I:8:16, 4>
//CHECK:     induction:
//CHECK:      <J:7:14, 4>:[Int,0,10,1]
//CHECK:     reduction:
//CHECK:      <S:4:10, 8>:min
//CHECK:     read only:
//CHECK:      <A, 8000> | <K:6:12, 4>
//CHECK:     lock:
//CHECK:      <J:7:14, 4>
//CHECK:     header access:
//CHECK:      <J:7:14, 4>
//CHECK:     explicit access:
//CHECK:      <I:8:16, 4> | <J:7:14, 4> | <K:6:12, 4> | <S:4:10, 8>
//CHECK:     explicit access (separate):
//CHECK:      <I:8:16, 4> <J:7:14, 4> <K:6:12, 4> <S:4:10, 8>
//CHECK:     lock (separate):
//CHECK:      <J:7:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <A, 8000> <I:8:16, 4> <J:7:14, 4> <K:6:12, 4> <S:4:10, 8>
//CHECK:    loop at depth 3 reduction_4.c:8:7
//CHECK:      induction:
//CHECK:       <I:8:16, 4>:[Int,0,10,1]
//CHECK:      reduction:
//CHECK:       <S:4:10, 8>:min
//CHECK:      read only:
//CHECK:       <A, 8000> | <J:7:14, 4> | <K:6:12, 4>
//CHECK:      lock:
//CHECK:       <I:8:16, 4>
//CHECK:      header access:
//CHECK:       <I:8:16, 4>
//CHECK:      explicit access:
//CHECK:       <I:8:16, 4> | <J:7:14, 4> | <K:6:12, 4> | <S:4:10, 8>
//CHECK:      explicit access (separate):
//CHECK:       <I:8:16, 4> <J:7:14, 4> <K:6:12, 4> <S:4:10, 8>
//CHECK:      lock (separate):
//CHECK:       <I:8:16, 4>
//CHECK:      direct access (separate):
//CHECK:       <A, 8000> <I:8:16, 4> <J:7:14, 4> <K:6:12, 4> <S:4:10, 8>
