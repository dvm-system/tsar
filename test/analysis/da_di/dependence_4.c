void foo(int *restrict N, float *restrict A) {
  for (int I = 1; I < N[0]; I = I + 2) {
    float V = A[I];
    for (int J = N[I]; J < N[I + 1]; ++J) {
      if (J < N[1]) {
        A[J] = A[J] + V;
      } else {
          // The inner 'for' loop does not actually contain this expression.
          // So, this situation produces a special analysis case.
          // Thus analyzer may assume absence of data dependencies
          // in the inner loop.
          A[J - 1] = N[I];
        break;
      }
    }
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 dependence_4.c:2:3
//CHECK:    private:
//CHECK:     <J:4:14, 4> | <V:3:11, 4>
//CHECK:    output:
//CHECK:     <*A:1:43, ?>
//CHECK:    anti:
//CHECK:     <*A:1:43, ?>
//CHECK:    flow:
//CHECK:     <*A:1:43, ?>
//CHECK:    induction:
//CHECK:     <I:2:12, 4>:[Int,1,,2]
//CHECK:    read only:
//CHECK:     <*N:1:24, ?> | <A:1:43, 8> | <N:1:24, 8>
//CHECK:    lock:
//CHECK:     <*N:1:24, ?> | <I:2:12, 4> | <N:1:24, 8>
//CHECK:    header access:
//CHECK:     <*N:1:24, ?> | <I:2:12, 4> | <N:1:24, 8>
//CHECK:    explicit access:
//CHECK:     <A:1:43, 8> | <I:2:12, 4> | <J:4:14, 4> | <N:1:24, 8> | <V:3:11, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:1:43, 8> <I:2:12, 4> <J:4:14, 4> <N:1:24, 8> <V:3:11, 4>
//CHECK:    lock (separate):
//CHECK:     <*N:1:24, ?> <I:2:12, 4> <N:1:24, 8>
//CHECK:    direct access (separate):
//CHECK:     <*A:1:43, ?> <*N:1:24, ?> <A:1:43, 8> <I:2:12, 4> <J:4:14, 4> <N:1:24, 8> <V:3:11, 4>
//CHECK:   loop at depth 2 dependence_4.c:4:5
//CHECK:     shared:
//CHECK:      <*A:1:43, ?>
//CHECK:     induction:
//CHECK:      <J:4:14, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <*N:1:24, ?> | <A:1:43, 8> | <I:2:12, 4> | <N:1:24, 8> | <V:3:11, 4>
//CHECK:     lock:
//CHECK:      <*N:1:24, ?> | <I:2:12, 4> | <J:4:14, 4> | <N:1:24, 8>
//CHECK:     header access:
//CHECK:      <*N:1:24, ?> | <I:2:12, 4> | <J:4:14, 4> | <N:1:24, 8>
//CHECK:     explicit access:
//CHECK:      <A:1:43, 8> | <I:2:12, 4> | <J:4:14, 4> | <N:1:24, 8> | <V:3:11, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:1:43, 8> <I:2:12, 4> <J:4:14, 4> <N:1:24, 8> <V:3:11, 4>
//CHECK:     lock (separate):
//CHECK:      <*N:1:24, ?> <I:2:12, 4> <J:4:14, 4> <N:1:24, 8>
//CHECK:     direct access (separate):
//CHECK:      <*A:1:43, ?> <*N:1:24, ?> <A:1:43, 8> <I:2:12, 4> <J:4:14, 4> <N:1:24, 8> <V:3:11, 4>
