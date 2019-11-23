int J;
void foo(int N, double *restrict A) {
#pragma omp parallel for default(shared) firstprivate(J) lastprivate(J)
  for (int I = 0; I < N; ++I) {
    J = N + I;
    A[I] = I + J;
  }
}
