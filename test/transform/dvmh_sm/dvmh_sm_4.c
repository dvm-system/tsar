enum { N = 100 };

double A[N];
void foo() {
  for (int I = 1; I <= N; ++I) {
    A[N - I] = I;
  }
}
//CHECK: dvmh_sm_4.c:5:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 1; I <= N; ++I) {
//CHECK:   ^
