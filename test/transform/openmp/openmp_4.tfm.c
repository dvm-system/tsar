void foo(int N, double *A) {
  int J;
#pragma omp parallel for default(shared) private(J)
  for (int I = 0; I < N; ++I) {
    J = N + I;
    A[I] = I + J;
  }
}
