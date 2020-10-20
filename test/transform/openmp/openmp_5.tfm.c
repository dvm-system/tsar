int J;
void foo(int N, double *restrict A) {
#pragma omp parallel
  {
#pragma omp for default(shared) lastprivate(J) firstprivate(J)
    for (int I = 0; I < N; ++I) {
      J = N + I;
      A[I] = I + J;
    }
  }
}
