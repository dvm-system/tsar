void bar(double *A) { A[5] = 10; }

void foo(int N) {
  double A[N][10];
  // Dependence may exist because bar() could add offset to its parameter
  // and the same call may access different rows of A.
  // Option '-finbounds-subscripts' has no sense in this case.
  for (int I = 0; I < 10; ++I)
    bar(A[I]);
}
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'bar':
//CHECK: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//CHECK:  loop at depth 1 dependence_1.c:8:3
//CHECK:    output:
//CHECK:     <A:{4:3|4:10}, ?> bar():9:5
//CHECK:    anti:
//CHECK:     <A:{4:3|4:10}, ?> bar():9:5
//CHECK:    flow:
//CHECK:     <A:{4:3|4:10}, ?> bar():9:5
//CHECK:    induction:
//CHECK:     <I:8:12, 4>:[Int,0,10,1]
//CHECK:    lock:
//CHECK:     <I:8:12, 4>
//CHECK:    header access:
//CHECK:     <A:{4:3|4:10}, ?> bar():9:5 | <I:8:12, 4>
//CHECK:    explicit access:
//CHECK:     <A:{4:3|4:10}, ?> bar():9:5 | <I:8:12, 4>
//CHECK:    explicit access (separate):
//CHECK:     <I:8:12, 4> bar():9:5
