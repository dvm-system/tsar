double U[100][100];

void foo() {
  int JStart = 0;
  for (int I = 1; I < 100; ++I) {
    for (int J = JStart; J < 99; ++J)
      U[I][J] = U[I - 1][J] + 1;
    ++JStart;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 distance_2.c:5:3
//CHECK:    private:
//CHECK:     <J:6:14, 4>
//CHECK:    flow:
//CHECK:     <U, 80000>:[1,1]
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,1,100,1] | <JStart:4:7, 4>:[Int,0,99,1]
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:5:12, 4> | <J:6:14, 4> | <JStart:4:7, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <J:6:14, 4> <JStart:4:7, 4>
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:5:12, 4> <J:6:14, 4> <JStart:4:7, 4> <U, 80000>
//CHECK:   loop at depth 2 distance_2.c:6:5
//CHECK:     shared:
//CHECK:      <U, 80000>
//CHECK:     induction:
//CHECK:      <J:6:14, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:5:12, 4>
//CHECK:     lock:
//CHECK:      <J:6:14, 4>
//CHECK:     header access:
//CHECK:      <J:6:14, 4>
//CHECK:     explicit access:
//CHECK:      <I:5:12, 4> | <J:6:14, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:5:12, 4> <J:6:14, 4>
//CHECK:     lock (separate):
//CHECK:      <J:6:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:5:12, 4> <J:6:14, 4> <U, 80000>
