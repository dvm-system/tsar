double U[100];

void foo() {
  for (int I = 1; I < 100; ++I)
    for (int J = 0; J < 5; ++J)
      U[I] = U[I - 1] + I;
}

//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 distance_6.c:4:3
//CHECK:    private:
//CHECK:     <J:5:14, 4>
//CHECK:    flow:
//CHECK:     <U, 800>:[1,1]
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,1,100,1]
//CHECK:    lock:
//CHECK:     <I:4:12, 4>
//CHECK:    header access:
//CHECK:     <I:4:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:4:12, 4> | <J:5:14, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:4:12, 4> <J:5:14, 4>
//CHECK:    lock (separate):
//CHECK:     <I:4:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:4:12, 4> <J:5:14, 4> <U, 800>
//CHECK:   loop at depth 2 distance_6.c:5:5
//CHECK:     output:
//CHECK:      <U, 800>
//CHECK:     induction:
//CHECK:      <J:5:14, 4>:[Int,0,5,1]
//CHECK:     read only:
//CHECK:      <I:4:12, 4>
//CHECK:     lock:
//CHECK:      <J:5:14, 4>
//CHECK:     header access:
//CHECK:      <J:5:14, 4>
//CHECK:     explicit access:
//CHECK:      <I:4:12, 4> | <J:5:14, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:4:12, 4> <J:5:14, 4>
//CHECK:     lock (separate):
//CHECK:      <J:5:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:4:12, 4> <J:5:14, 4> <U, 800>
