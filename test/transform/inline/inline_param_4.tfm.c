void foo(int N, double (*A)[N / 2 * 2 + 2]) {
  double(*B)[N / 2 * 2 + 2] = A;
  for (int I = 0; I < N / 2 * 2 + 2; ++I)
    for (int J = 0; J < N / 2 * 2 + 2; ++J)
      B[I][J] = I + J;
}

void bar() {
  double A[10][10];

  /* foo(8, A) is inlined below */
#pragma spf assert nomacro
  {
    int N0 = 8;
    double(*A0)[N0 / 2 * 2 + 2] = A;
    double(*B)[N0 / 2 * 2 + 2] = A0;
    for (int I = 0; I < N0 / 2 * 2 + 2; ++I)
      for (int J = 0; J < N0 / 2 * 2 + 2; ++J)
        B[I][J] = I + J;
  }
}
