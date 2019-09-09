int JStart;
void bar();

double foo() {
  double U[100];
  int I, J;
  for (int I = 0; I < 100; I = I + 10) {
    // This call may change global variable JStart.
    // So, JStart is not invariant for the outher loop
    // and analysis will fail.
    bar();
    for (J = JStart; J < 10; ++J)
      U[I + J] = U[I + J] + 1;
  }
  return U[50];
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 shared_12.c:7:3
//CHECK:    private:
//CHECK:     <J:6:10, 4>
//CHECK:    output:
//CHECK:     <JStart, 4> bar():11:5 | <U:5:10, 800>
//CHECK:    anti:
//CHECK:     <JStart, 4> bar():11:5 | <U:5:10, 800>
//CHECK:    flow:
//CHECK:     <JStart, 4> bar():11:5 | <U:5:10, 800>
//CHECK:    induction:
//CHECK:     <I:7:12, 4>:[Int,0,100,10]
//CHECK:    lock:
//CHECK:     <I:7:12, 4>
//CHECK:    header access:
//CHECK:     <I:7:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:7:12, 4> | <J:6:10, 4> | <JStart, 4> bar():11:5
//CHECK:    address access:
//CHECK:     <JStart, 4> bar():11:5
//CHECK:    explicit access (separate):
//CHECK:     <I:7:12, 4> <J:6:10, 4> <JStart, 4> bar():11:5
//CHECK:    lock (separate):
//CHECK:     <I:7:12, 4>
//CHECK:    address access (separate):
//CHECK:     bar():11:5
//CHECK:    direct access (separate):
//CHECK:     <I:7:12, 4> <J:6:10, 4> <JStart, 4> <U:5:10, 800> bar():11:5
//CHECK:   loop at depth 2 shared_12.c:12:5
//CHECK:     shared:
//CHECK:      <U:5:10, 800>
//CHECK:     induction:
//CHECK:      <J:6:10, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:7:12, 4>
//CHECK:     lock:
//CHECK:      <J:6:10, 4>
//CHECK:     header access:
//CHECK:      <J:6:10, 4>
//CHECK:     explicit access:
//CHECK:      <I:7:12, 4> | <J:6:10, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:7:12, 4> <J:6:10, 4>
//CHECK:     lock (separate):
//CHECK:      <J:6:10, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:7:12, 4> <J:6:10, 4> <U:5:10, 800>
