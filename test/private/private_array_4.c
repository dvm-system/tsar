void foo(double *X, int N) {
  double T[3] = {0, N, 2};
  for (int I = 1; I < N - 1; ++I) {
      T[1] = X[I-1];
      T[2] = X[I+1];
      X[I] += T[0] * T[1] + T[2];
    }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_array_4.c:3:3
//CHECK:    first private:
//CHECK:     <T:2:10, 24>
//CHECK:    anti:
//CHECK:     <*X:1:18, ?>:[1,1]
//CHECK:    flow:
//CHECK:     <*X:1:18, ?>:[1,1]
//CHECK:    induction:
//CHECK:     <I:3:12, 4>:[Int,1,,1]
//CHECK:    read only:
//CHECK:     <N:1:25, 4> | <X:1:18, 8>
//CHECK:    redundant:
//CHECK:     <T:2:10, 24> <T:2:10, ?>
//CHECK:    lock:
//CHECK:     <I:3:12, 4> | <N:1:25, 4>
//CHECK:    header access:
//CHECK:     <*X:1:18, ?> | <I:3:12, 4> | <N:1:25, 4>
//CHECK:    explicit access:
//CHECK:     <I:3:12, 4> | <N:1:25, 4> | <X:1:18, 8>
//CHECK:    explicit access (separate):
//CHECK:     <I:3:12, 4> <N:1:25, 4> <X:1:18, 8>
//CHECK:    redundant (separate):
//CHECK:     <T:2:10, ?>
//CHECK:    lock (separate):
//CHECK:     <I:3:12, 4> <N:1:25, 4>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 private_array_4.c:3:3
//SAFE:    output:
//SAFE:     <T:2:10, 24> <T:2:10, ?>
//SAFE:    anti:
//SAFE:     <*X:1:18, ?>:[1,1] | <T:2:10, 24> <T:2:10, ?>
//SAFE:    flow:
//SAFE:     <*X:1:18, ?>:[1,1] | <T:2:10, 24> <T:2:10, ?>
//SAFE:    induction:
//SAFE:     <I:3:12, 4>:[Int,1,,1]
//SAFE:    read only:
//SAFE:     <N:1:25, 4> | <X:1:18, 8>
//SAFE:    redundant:
//SAFE:     <T:2:10, 24> <T:2:10, ?>
//SAFE:    lock:
//SAFE:     <I:3:12, 4> | <N:1:25, 4>
//SAFE:    header access:
//SAFE:     <*X:1:18, ?> | <I:3:12, 4> | <N:1:25, 4>
//SAFE:    explicit access:
//SAFE:     <I:3:12, 4> | <N:1:25, 4> | <X:1:18, 8>
//SAFE:    explicit access (separate):
//SAFE:     <I:3:12, 4> <N:1:25, 4> <X:1:18, 8>
//SAFE:    redundant (separate):
//SAFE:     <T:2:10, ?>
//SAFE:    lock (separate):
//SAFE:     <I:3:12, 4> <N:1:25, 4>
