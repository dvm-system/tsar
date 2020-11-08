void foo(int *X, int *Y, int Z) {
   for (int K = 0; K < 10; ++K)
     *X = *X + *Y + Z;
}

void bar(int *A) {
  for (int I = 0; I < 100; ++I) {
    int X = I;
    foo(&A[I], &I, X);
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 interproc_9.c:2:4
//CHECK:    output:
//CHECK:     <X[0]:1, 4> <Y[0]:1, 4>
//CHECK:    anti:
//CHECK:     <X[0]:1, 4> <Y[0]:1, 4>
//CHECK:    flow:
//CHECK:     <X[0]:1, 4> <Y[0]:1, 4>
//CHECK:    induction:
//CHECK:     <K:2[2:4], 4>:[Int,0,10,1]
//CHECK:    read only:
//CHECK:     <X:1, 8> | <Y:1, 8> | <Z:1, 4>
//CHECK:    lock:
//CHECK:     <K:2[2:4], 4>
//CHECK:    header access:
//CHECK:     <K:2[2:4], 4>
//CHECK:    explicit access:
//CHECK:     <K:2[2:4], 4> | <X:1, 8> | <X[0]:1, 4> <Y[0]:1, 4> | <Y:1, 8> | <Z:1, 4>
//CHECK:    explicit access (separate):
//CHECK:     <K:2[2:4], 4> <X:1, 8> <X[0]:1, 4> <Y:1, 8> <Y[0]:1, 4> <Z:1, 4>
//CHECK:    lock (separate):
//CHECK:     <K:2[2:4], 4>
//CHECK:    direct access (separate):
//CHECK:     <K:2[2:4], 4> <X:1, 8> <X[0]:1, 4> <Y:1, 8> <Y[0]:1, 4> <Z:1, 4>
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK:  loop at depth 1 interproc_9.c:7:3
//CHECK:    shared:
//CHECK:     <*A:6, ?>
//CHECK:    private:
//CHECK:     <X:8[7:33], 4>
//CHECK:    induction:
//CHECK:     <I:7[7:3], 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <A:6, 8>
//CHECK:    lock:
//CHECK:     <I:7[7:3], 4>
//CHECK:    header access:
//CHECK:     <I:7[7:3], 4>
//CHECK:    explicit access:
//CHECK:     <A:6, 8> | <I:7[7:3], 4> | <X:8[7:33], 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:6, 8> <I:7[7:3], 4> <X:8[7:33], 4>
//CHECK:    lock (separate):
//CHECK:     <I:7[7:3], 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:6, ?> <A:6, 8> <I:7[7:3], 4> <X:8[7:33], 4>
