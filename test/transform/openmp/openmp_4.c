void foo(int N, double *A) {
  int J;
  for (int I = 0; I < N; ++I) {
    J = N +I;
    A[I] = I + J;
  }
}
//CHECK: openmp_4.c:3:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I) {
//CHECK:   ^
