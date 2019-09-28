double U[100][100];

void foo() {
  for (int I = 2; I < 100; ++I)
    for (int J = 0; J < 99; ++J)
      U[I][J] = U[I-1][J] + U[I - 2][J + 1];
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 distance_1.c:4:3
//CHECK:    private:
//CHECK:     <J:5:14, 4>
//CHECK:    flow:
//CHECK:     <U, 80000>:[1,2]
//CHECK:    induction:
//CHECK:     <I:4:12, 4>:[Int,2,100,1]
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
//CHECK:     <I:4:12, 4> <J:5:14, 4> <U, 80000>
//CHECK:   loop at depth 2 distance_1.c:5:5
//CHECK:     shared:
//CHECK:      <U, 80000>
//CHECK:     induction:
//CHECK:      <J:5:14, 4>:[Int,0,99,1]
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
//CHECK:      <I:4:12, 4> <J:5:14, 4> <U, 80000>
