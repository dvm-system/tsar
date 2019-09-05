void foo(int *A) {
  double T[2][2];
  for (int I = 0; I < 100; ++I) { 
    T[0][0] = I;
    T[0][1] = I + 1;
    T[1][0] = I;
    T[1][1] = I - 1;
    for (int J = 0; J < 2; ++J)
      A[I] = T[0][J] * T[1][J];
    A[I] = A[I] + A[I+1];
  }
}

//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 private_array_7.c:3:3
//CHECK:    private:
//CHECK:     <J:8:14, 4> | <T:2:10, 32>
//CHECK:    anti:
//CHECK:     <*A:1:15, ?>:[1,1]
//CHECK:    induction:
//CHECK:     <I:3:12, 4>:[Int,0,100,1]
//CHECK:    read only:
//CHECK:     <A:1:15, 8>
//CHECK:    lock:
//CHECK:     <I:3:12, 4>
//CHECK:    header access:
//CHECK:     <I:3:12, 4>
//CHECK:    explicit access:
//CHECK:     <A:1:15, 8> | <I:3:12, 4> | <J:8:14, 4>
//CHECK:    explicit access (separate):
//CHECK:     <A:1:15, 8> <I:3:12, 4> <J:8:14, 4>
//CHECK:    lock (separate):
//CHECK:     <I:3:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <*A:1:15, ?> <A:1:15, 8> <I:3:12, 4> <J:8:14, 4> <T:2:10, 32>
//CHECK:   loop at depth 2 private_array_7.c:8:5
//CHECK:     first private:
//CHECK:      <*A:1:15, ?>
//CHECK:     second to last private:
//CHECK:      <*A:1:15, ?>
//CHECK:     induction:
//CHECK:      <J:8:14, 4>:[Int,0,2,1]
//CHECK:     read only:
//CHECK:      <A:1:15, 8> | <I:3:12, 4> | <T:2:10, 32>
//CHECK:     lock:
//CHECK:      <J:8:14, 4>
//CHECK:     header access:
//CHECK:      <J:8:14, 4>
//CHECK:     explicit access:
//CHECK:      <A:1:15, 8> | <I:3:12, 4> | <J:8:14, 4>
//CHECK:     explicit access (separate):
//CHECK:      <A:1:15, 8> <I:3:12, 4> <J:8:14, 4>
//CHECK:     lock (separate):
//CHECK:      <J:8:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <*A:1:15, ?> <A:1:15, 8> <I:3:12, 4> <J:8:14, 4> <T:2:10, 32>
//SAFE: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//SAFE:  loop at depth 1 private_array_7.c:3:3
//SAFE:    private:
//SAFE:     <J:8:14, 4>
//SAFE:    output:
//SAFE:     <T:2:10, 32>
//SAFE:    anti:
//SAFE:     <*A:1:15, ?>:[1,1] | <T:2:10, 32>
//SAFE:    flow:
//SAFE:     <T:2:10, 32>
//SAFE:    induction:
//SAFE:     <I:3:12, 4>:[Int,0,100,1]
//SAFE:    read only:
//SAFE:     <A:1:15, 8>
//SAFE:    lock:
//SAFE:     <I:3:12, 4>
//SAFE:    header access:
//SAFE:     <I:3:12, 4>
//SAFE:    explicit access:
//SAFE:     <A:1:15, 8> | <I:3:12, 4> | <J:8:14, 4>
//SAFE:    explicit access (separate):
//SAFE:     <A:1:15, 8> <I:3:12, 4> <J:8:14, 4>
//SAFE:    lock (separate):
//SAFE:     <I:3:12, 4>
//SAFE:    direct access (separate):
//SAFE:     <*A:1:15, ?> <A:1:15, 8> <I:3:12, 4> <J:8:14, 4> <T:2:10, 32>
//SAFE:   loop at depth 2 private_array_7.c:8:5
//SAFE:     first private:
//SAFE:      <*A:1:15, ?>
//SAFE:     second to last private:
//SAFE:      <*A:1:15, ?>
//SAFE:     induction:
//SAFE:      <J:8:14, 4>:[Int,0,2,1]
//SAFE:     read only:
//SAFE:      <A:1:15, 8> | <I:3:12, 4> | <T:2:10, 32>
//SAFE:     lock:
//SAFE:      <J:8:14, 4>
//SAFE:     header access:
//SAFE:      <J:8:14, 4>
//SAFE:     explicit access:
//SAFE:      <A:1:15, 8> | <I:3:12, 4> | <J:8:14, 4>
//SAFE:     explicit access (separate):
//SAFE:      <A:1:15, 8> <I:3:12, 4> <J:8:14, 4>
//SAFE:     lock (separate):
//SAFE:      <J:8:14, 4>
//SAFE:     direct access (separate):
//SAFE:      <*A:1:15, ?> <A:1:15, 8> <I:3:12, 4> <J:8:14, 4> <T:2:10, 32>
