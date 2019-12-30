int bar() { return 10; };

void foo(int N, double *A) {
  int X, Y;
#pragma omp parallel for default(shared) private(X, Y)
  for (int I = 0; I < N; ++I) {
    X = I;
    if (I > N) {
      Y = bar();
      X = X + Y + 1;
    }
    A[I] = X;
  }
}
