int JStart;
void bar();

double foo() {
  double U[100];
  for (int I = 1; I < 100; ++I) {
    bar();
    for (int J = JStart; J < 5; ++J)
      U[I] = U[I - 1] + I;
  }
  return U[50];
}

//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 distance_7.c:6:3
//CHECK:    private:
//CHECK:     <J:8:14, 4>
//CHECK:    output:
//CHECK:     <JStart, 4> bar():7:5
//CHECK:    anti:
//CHECK:     <JStart, 4> bar():7:5
//CHECK:    flow:
//CHECK:     <JStart, 4> bar():7:5 | <U:5:10, 800>:[1,1]
//CHECK:    induction:
//CHECK:     <I:6:12, 4>:[Int,1,100,1]
//CHECK:    lock:
//CHECK:     <I:6:12, 4>
//CHECK:    header access:
//CHECK:     <I:6:12, 4>
//CHECK:    explicit access:
//CHECK:     <I:6:12, 4> | <J:8:14, 4> | <JStart, 4> bar():7:5
//CHECK:    address access:
//CHECK:     <JStart, 4> bar():7:5
//CHECK:    explicit access (separate):
//CHECK:     <I:6:12, 4> <J:8:14, 4> <JStart, 4> bar():7:5
//CHECK:    lock (separate):
//CHECK:     <I:6:12, 4>
//CHECK:    address access (separate):
//CHECK:     bar():7:5
//CHECK:    direct access (separate):
//CHECK:     <I:6:12, 4> <J:8:14, 4> <JStart, 4> <U:5:10, 800> bar():7:5
//CHECK:   loop at depth 2 distance_7.c:8:5
//CHECK:     output:
//CHECK:      <U:5:10, 800>
//CHECK:     induction:
//CHECK:      <J:8:14, 4>:[Int,,,1]
//CHECK:     read only:
//CHECK:      <I:6:12, 4>
//CHECK:     lock:
//CHECK:      <J:8:14, 4>
//CHECK:     header access:
//CHECK:      <J:8:14, 4>
//CHECK:     explicit access:
//CHECK:      <I:6:12, 4> | <J:8:14, 4>
//CHECK:     explicit access (separate):
//CHECK:      <I:6:12, 4> <J:8:14, 4>
//CHECK:     lock (separate):
//CHECK:      <J:8:14, 4>
//CHECK:     direct access (separate):
//CHECK:      <I:6:12, 4> <J:8:14, 4> <U:5:10, 800>
