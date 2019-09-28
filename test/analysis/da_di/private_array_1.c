void foo(double *X, int N) {
  double T[2];
  for (int I = 1; I < N - 1; ++I) {
    T[0] = X[I-1];
    T[1] = X[I+1];
    X[I] += T[0] + T[1];
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_array_1.c:3:3
//CHECK:    private:
//CHECK:     <T:2:10, 16>
//CHECK:    anti:
//CHECK:     <*X:1:18, ?>:[1,1]
//CHECK:    flow:
//CHECK:     <*X:1:18, ?>:[1,1]
//CHECK:    induction:
//CHECK:     <I:3:12, 4>:[Int,1,,1]
//CHECK:    read only:
//CHECK:     <N:1:25, 4> | <X:1:18, 8>
//CHECK:    lock:
//CHECK:     <I:3:12, 4> | <N:1:25, 4>
//CHECK:    header access:
//CHECK:     <I:3:12, 4> | <N:1:25, 4>
//CHECK:    explicit access:
//CHECK:     <I:3:12, 4> | <N:1:25, 4> | <X:1:18, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:3:12, 4> <N:1:25, 4> <X:1:18, 8>
//CHECK:    lock (separate):
//CHECK:     <I:3:12, 4> <N:1:25, 4>
//CHECK:    direct access (separate):
//CHECK:     <*X:1:18, ?> <I:3:12, 4> <N:1:25, 4> <T:2:10, 16> <X:1:18, 8>
