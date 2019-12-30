void foo(int N, double *A) {
  int J = N;
  for (int I = 0; I < N; ++I) {
    ++J;
    A[I] = I + J;
  }
}
//CHECK: openmp_3.c:3:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I) {
//CHECK:   ^
//CHECK: openmp_3.c:3:3: warning: unable to create parallel directive
//CHECK: openmp_3.c:3:12: note: loop has multiple inducition variables
//CHECK:   for (int I = 0; I < N; ++I) {
//CHECK:            ^
//CHECK: openmp_3.c:2:7: note: loop has multiple inducition variables
//CHECK:   int J = N;
//CHECK:       ^
//CHECK: 1 warning generated.
