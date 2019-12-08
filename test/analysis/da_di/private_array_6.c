void foo(int *A) {
  for (int I = 0; I < 100; ++I) {
    double T[2][2];
    T[0][0] = I;
    T[0][1] = I + 1;
    T[1][0] = I;
    T[1][1] = I - 1;
    for (int J = 0; J < 2; ++J)
      A[I] *= T[0][J] * T[1][J];
  }
  
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_array_6.c:2:3
//CHECK:    shared:
//CHECK:     <*A:1:15, ?>
//CHECK:    private:
//CHECK:     <J:8:14, 4> | <T:3:12, 32>
//CHECK:    induction:
//CHECK:     <I:2:12, 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <A:1:15, 8>
//CHECK:    lock:
//CHECK:     <I:2:12, 4>
//CHECK:    header access:
//CHECK:     <I:2:12, 4>
//CHECK:    explicit access:
//CHECK:     <A:1:15, 8> | <I:2:12, 4> | <J:8:14, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:1:15, 8> <I:2:12, 4> <J:8:14, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:1:15, ?> <A:1:15, 8> <I:2:12, 4> <J:8:14, 4> <T:3:12, 32>
//CHECK:   loop at depth 2 private_array_6.c:8:5
//CHECK:     output:
//CHECK:      <*A:1:15, ?>
//CHECK:     anti:
//CHECK:      <*A:1:15, ?>
//CHECK:     flow:
//CHECK:      <*A:1:15, ?>
//CHECK:     induction:
//CHECK:      <J:8:14, 4>:[Int,0,2,1]
//CHECK:     read only:
//CHECK:      <A:1:15, 8> | <I:2:12, 4> | <T:3:12, 32>
//CHECK:     lock:
//CHECK:      <J:8:14, 4>
//CHECK:     header access:
//CHECK:      <J:8:14, 4>
//CHECK:     explicit access:
//CHECK:      <A:1:15, 8> | <I:2:12, 4> | <J:8:14, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:1:15, 8> <I:2:12, 4> <J:8:14, 4>
//CHECK:     lock (separate):
//CHECK:      <J:8:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:1:15, ?> <A:1:15, 8> <I:2:12, 4> <J:8:14, 4> <T:3:12, 32>
