void foo(int N, double *restrict X, float * restrict A) {
   for (int I = 0; I < 10; ++I) {
     *X = I;
     A[I] = *X;   
   }
}

int bar(int N, float * restrict A) {
  double X = N;
  foo(N, &X, A);
  return N;
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 interproc_5.c:2:4
//CHECK:    shared:
//CHECK:     <*A:1:54, ?>
//CHECK:    private:
//CHECK:     <*A:1:54, ?> | <*X:1:34, 8>
//CHECK:    induction:
//CHECK:     <I:2:13, 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <A:1:54, 8> | <X:1:34, 8>
//CHECK:    lock:
//CHECK:     <I:2:13, 4>
//CHECK:    header access:
//CHECK:     <I:2:13, 4>
//CHECK:    explicit access:
//CHECK:     <A:1:54, 8> | <I:2:13, 4> | <X:1:34, 8>
//CHECK:    explicit access (separate):
//CHECK:     <*X:1:34, 8> <A:1:54, 8> <I:2:13, 4> <X:1:34, 8>
//CHECK:    lock (separate):
//CHECK:     <I:2:13, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:1:54, ?> <*X:1:34, 8> <A:1:54, 8> <I:2:13, 4> <X:1:34, 8>
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
