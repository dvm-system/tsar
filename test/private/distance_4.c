double U[100][100];

void foo() {
  int JStart = 0;
  for (int I = 1; I < 99; ++I) {
    ++JStart;
    for (int J = JStart; J < 99; ++J)
      U[I][J] = U[I - 1][J] + 1;
  }
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 distance_4.c:5:3
//CHECK:    private:
//CHECK:     <J:7:14, 4>
//CHECK:    flow:
//CHECK:     <U, 80000>:[1,1]
//CHECK:    induction:
//CHECK:     <I:5:12, 4>:[Int,1,99,1] | <JStart:4:7, 4>:[Int,0,98,1]
//CHECK:    lock:
//CHECK:     <I:5:12, 4>
//CHECK:    header access:
//CHECK:     <I:5:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:5:12, 4> | <J:7:14, 4> | <JStart:4:7, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:5:12, 4> <J:7:14, 4> <JStart:4:7, 4>
//CHECK:    lock (separate):
//CHECK:     <I:5:12, 4>
//CHECK:    direct access (separate):
//CHECK:     <I:5:12, 4> <J:7:14, 4> <JStart:4:7, 4> <U, 80000>
//CHECK:   loop at depth 2 distance_4.c:7:5
//CHECK:     shared:
//CHECK:      <U, 80000>
//CHECK:     induction:
//CHECK:      <J:7:14, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:5:12, 4>
//CHECK:     lock:
//CHECK:      <J:7:14, 4>
//CHECK:     header access:
//CHECK:      <J:7:14, 4>
//CHECK:     explicit access:
//CHECK:      <I:5:12, 4> | <J:7:14, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:5:12, 4> <J:7:14, 4>
//CHECK:     lock (separate):
//CHECK:      <J:7:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:5:12, 4> <J:7:14, 4> <U, 80000>
