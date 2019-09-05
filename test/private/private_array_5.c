void foo(int *A) {
  for (int I = 0; I < 100; ++I) {
    double T[2][3][4];
    T[0][0][0] = 0;
    T[0][1][1] = 1;
    T[1][0][2] = 1;
    T[1][1][3] = 2;
    A[I] = T[0][0][0] + T[0][1][1] * I + T[1][0][2] * I + T[1][1][3] * I * I;
  }
  
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_array_5.c:2:3
//CHECK:    shared:
//CHECK:     <*A:1:15, ?>
//CHECK:    first private:
//CHECK:     <*A:1:15, ?>
//CHECK:    dynamic private:
//CHECK:     <*A:1:15, ?>
//CHECK:    private:
//CHECK:     <T:3:12, 192>
//CHECK:    induction:
//CHECK:     <I:2:12, 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <A:1:15, 8>
//CHECK:    lock:
//CHECK:     <I:2:12, 4>
//CHECK:    header access:
//CHECK:     <I:2:12, 4>
//CHECK:    explicit access:
//CHECK:     <A:1:15, 8> | <I:2:12, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:1:15, 8> <I:2:12, 4>
//CHECK:    lock (separate):
//CHECK:     <I:2:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:1:15, ?> <A:1:15, 8> <I:2:12, 4> <T:3:12, 192>
