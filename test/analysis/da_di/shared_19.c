double foo() {
  double U[100];
  int I, J;
  for (int I = 0; I < 100; I = I + 10) {
    for (J = 0; J < 10; ++J)
      U[I + J] = U[I + J] + 1;
  }
  return U[50];
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_19.c:4:3
//CHECK:    shared:
//CHECK:     <U:2:10, 800>
//CHECK:    private:
//CHECK:     <J:3:10, 4>
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,0,100,10]
//CHECK:    lock:
//CHECK:     <I:4:12, 4>
//CHECK:    header access:
//CHECK:     <I:4:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:4:12, 4> | <J:3:10, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:4:12, 4> <J:3:10, 4>
//CHECK:    lock (separate):
//CHECK:     <I:4:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:4:12, 4> <J:3:10, 4> <U:2:10, 800>
//CHECK:   loop at depth 2 shared_19.c:5:5
//CHECK:     shared:
//CHECK:      <U:2:10, 800>
//CHECK:     induction:
//CHECK:      <J:3:10, 4>:[Int,0,10,1]
//CHECK:     read only:
//CHECK:      <I:4:12, 4>
//CHECK:     lock:
//CHECK:      <J:3:10, 4>
//CHECK:     header access:
//CHECK:      <J:3:10, 4>
//CHECK:     explicit access:
//CHECK:      <I:4:12, 4> | <J:3:10, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:4:12, 4> <J:3:10, 4>
//CHECK:     lock (separate):
//CHECK:      <J:3:10, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:4:12, 4> <J:3:10, 4> <U:2:10, 800>
