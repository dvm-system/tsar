void foo(int N, double *A) {
  int J = N;
  for (int I = 0; I < N; ++I, ++J) {
    A[I] = I + J;
  }
}
//CHECK: openmp_2.c:3:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I, ++J) {
//CHECK:   ^
//CHECK: openmp_2.c:3:3: warning: unable to create parallel directive for loop not in canonical form
//CHECK: 1 warning generated.
