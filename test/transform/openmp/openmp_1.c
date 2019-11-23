void foo(int N, double *A) {
  for (int I = 0; I < N; ++I) {
    A[I] = I;
  }
}
//CHECK: openmp_1.c:2:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I) {
//CHECK:   ^
