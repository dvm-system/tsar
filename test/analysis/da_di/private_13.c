long long N;

void foo(double * restrict X, int * restrict A) {
  for (int I = 0; I < N; ++I) {
    A[I] = A[I] + 1;
    *(X + N) = A[I];
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_13.c:4:3
//CHECK:    shared:
//CHECK:     <*A:3, ?>
//CHECK:    first private:
//CHECK:     <*X:3, ?>
//CHECK:    second to last private:
//CHECK:     <*X:3, ?>
//CHECK:    induction:
//CHECK:     <I:4[4:3], 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:3, 8> | <N, 8> | <X:3, 8>
//CHECK:    lock:
//CHECK:     <I:4[4:3], 4> | <N, 8>
//CHECK:    header access:
//CHECK:     <I:4[4:3], 4> | <N, 8>
//CHECK:    explicit access:
//CHECK:     <A:3, 8> | <I:4[4:3], 4> | <N, 8> | <X:3, 8>
//CHECK:    explicit access (separate):
//CHECK:     <A:3, 8> <I:4[4:3], 4> <N, 8> <X:3, 8>
//CHECK:    lock (separate):
//CHECK:     <I:4[4:3], 4> <N, 8>
//CHECK:    direct access (separate):
//CHECK:     <*A:3, ?> <*X:3, ?> <A:3, 8> <I:4[4:3], 4> <N, 8> <X:3, 8>
