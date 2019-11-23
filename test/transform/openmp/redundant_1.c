int bar() { return 10; };

void foo(int N, double *A) {
  int X, Y;
  for (int I = 0; I < N; ++I) {
    X = I;
    if (I > N) {
      Y = bar();
      X = X + Y + 1;
    }
    A[I] = X;
  }
}
//CHECK: redundant_1.c:5:3: remark: parallel execution of loop is possible
//CHECK:   for (int I = 0; I < N; ++I) {
//CHECK:   ^
