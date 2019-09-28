double foo(double *X, int N) {
  double T[3];
  T[0] = N;
  for (int I = 1; I < N - 1; ++I) {
      T[1] = X[I-1];
      T[2] = X[I+1];
      X[I] += T[0] * T[1] + T[2];
    }
  return T[0] * T[1] + T[2];
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_array_3.c:4:3
//CHECK:    first private:
//CHECK:     <T:2:10, 24>
//CHECK:    second to last private:
//CHECK:     <T:2:10, 24>
//CHECK:    anti:
//CHECK:     <*X:1:20, ?>:[1,1]
//CHECK:    flow:
//CHECK:     <*X:1:20, ?>:[1,1]
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,1,,1]
//CHECK:    read only:
//CHECK:     <N:1:27, 4> | <X:1:20, 8>
//CHECK:    lock:
//CHECK:     <I:4:12, 4> | <N:1:27, 4>
//CHECK:    header access:
//CHECK:     <I:4:12, 4> | <N:1:27, 4>
//CHECK:    explicit access:
//CHECK:     <I:4:12, 4> | <N:1:27, 4> | <X:1:20, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:4:12, 4> <N:1:27, 4> <X:1:20, 8>
//CHECK:    lock (separate):
//CHECK:     <I:4:12, 4> <N:1:27, 4>
//CHECK:    direct access (separate):
//CHECK:     <*X:1:20, ?> <I:4:12, 4> <N:1:27, 4> <T:2:10, 24> <X:1:20, 8>
