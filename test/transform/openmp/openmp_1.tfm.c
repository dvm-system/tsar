void foo(int N, double *A) {
#pragma omp parallel for default(shared)
  for (int I = 0; I < N; ++I) {
    A[I] = I;
  }
}
