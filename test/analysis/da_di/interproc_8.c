void bar(int I, int *X) {
  *X = I;
}

void foo(int N, int *A) {
  int X;
  for (int I = 0; I < N; ++I) {
    bar(I, &X);
    A[I] = X;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 interproc_8.c:7:3
//CHECK:    shared:
//CHECK:     <*A:5, ?>
//CHECK:    first private:
//CHECK:     <*A:5, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:5, ?>
//CHECK:    private:
//CHECK:     <X:6, 4>
//CHECK:    induction:
//CHECK:     <I:7[7:3], 4>:[Int,0,,1]
//CHECK:    read only:
//CHECK:     <A:5, 8> | <N:5, 4>
//CHECK:    lock:
//CHECK:     <I:7[7:3], 4> | <N:5, 4>
//CHECK:    header access:
//CHECK:     <I:7[7:3], 4> | <N:5, 4>
//CHECK:    explicit access:
//CHECK:     <A:5, 8> | <I:7[7:3], 4> | <N:5, 4> | <X:6, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:5, 8> <I:7[7:3], 4> <N:5, 4> <X:6, 4>
//CHECK:    lock (separate):
//CHECK:     <I:7[7:3], 4> <N:5, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:5, ?> <A:5, 8> <I:7[7:3], 4> <N:5, 4> <X:6, 4>
