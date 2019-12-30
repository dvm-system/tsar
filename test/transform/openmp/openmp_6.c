double foo(int N, double * restrict A) {
  double S = 0;
  for (int I = 0; I < N; ++I) {
    S += A[I];
    A[I] = I;
  }
  return S;
}
//CHECK: openmp_6.c:3:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I) {
//CHECK:   ^
