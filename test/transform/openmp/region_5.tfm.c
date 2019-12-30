void foo(int N, int *A) {
  int TSize = 4;
  int T[4];
#pragma spf region name(ignore)
  for (int I = 0; I < TSize; ++I)
    T[I] = I;
#pragma spf region name(parallel)
#pragma omp parallel for default(shared)
  for (int I = 0; I < N; ++I) {
    A[I] = I;
    for (int J = 0; J < TSize; ++J)
      A[I] = A[I] + T[J];
  }
}
