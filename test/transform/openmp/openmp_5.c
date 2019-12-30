int J;
void foo(int N, double * restrict A) {
  for (int I = 0; I < N; ++I) {
    J = N + I;
    A[I] = I + J;
  }
}
//CHECK: openmp_5.c:3:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I) {
//CHECK:   ^
