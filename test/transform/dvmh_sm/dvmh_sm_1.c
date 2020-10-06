double A[100];
void foo(int N) {
  for (int I = 0; I < N; ++I) {
    A[I] = I;
  }
}
//CHECK: dvmh_sm_1.c:3:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I) {
//CHECK:   ^
