void foo(int N, int M, int K, float *A) {
  for (int I = 0; I < K; ++I) {
    float V = A[I];
    for (int J = K; J < N; ++J) {
      if (J < M) {
        A[J] = A[J] + V;
      } else {
        // The inner 'for' loop does not actually contain this expression.
        // So, this situation produces a special analysis case.
        // Thus analyzer may assume absence of data dependencies
        // in the inner loop.
        A[J - 1] = M;
        break;
      }
    }
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 dependence_3.c:2:3
//CHECK:    private:
//CHECK:     <J:4:14, 4> | <V:3:11, 4>
//CHECK:    output:
//CHECK:     <*A:1:38, ?>
//CHECK:    anti:
//CHECK:     <*A:1:38, ?>
//CHECK:    flow:
//CHECK:     <*A:1:38, ?>
//CHECK:    induction:
//CHECK:     <I:2:12, 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:1:38, 8> | <K:1:28, 4> | <M:1:21, 4> | <N:1:14, 4>
//CHECK:    lock:
//CHECK:     <I:2:12, 4> | <K:1:28, 4>
//CHECK:    header access:
//CHECK:     <I:2:12, 4> | <K:1:28, 4>
//CHECK:    explicit access:
//CHECK:     <A:1:38, 8> | <I:2:12, 4> | <J:4:14, 4> | <K:1:28, 4> | <M:1:21, 4> | <N:1:14, 4> | <V:3:11, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:1:38, 8> <I:2:12, 4> <J:4:14, 4> <K:1:28, 4> <M:1:21, 4> <N:1:14, 4> <V:3:11, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:12, 4> <K:1:28, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:1:38, ?> <A:1:38, 8> <I:2:12, 4> <J:4:14, 4> <K:1:28, 4> <M:1:21, 4> <N:1:14, 4> <V:3:11, 4>
//CHECK:   loop at depth 2 dependence_3.c:4:5
//CHECK:     shared:
//CHECK:      <*A:1:38, ?>
//CHECK:     induction:
//CHECK:      <J:4:14, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <A:1:38, 8> | <M:1:21, 4> | <N:1:14, 4> | <V:3:11, 4>
//CHECK:     lock:
//CHECK:      <J:4:14, 4> | <N:1:14, 4>
//CHECK:     header access:
//CHECK:      <J:4:14, 4> | <N:1:14, 4>
//CHECK:     explicit access:
//CHECK:      <A:1:38, 8> | <J:4:14, 4> | <M:1:21, 4> | <N:1:14, 4> | <V:3:11, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:1:38, 8> <J:4:14, 4> <M:1:21, 4> <N:1:14, 4> <V:3:11, 4>
//CHECK:     lock (separate):
//CHECK:      <J:4:14, 4> <N:1:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:1:38, ?> <A:1:38, 8> <J:4:14, 4> <M:1:21, 4> <N:1:14, 4> <V:3:11, 4>
