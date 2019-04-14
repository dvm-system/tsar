void foo(int N, double (*A)[N]) {
  for (int I = 0; I < N; ++I)
    for (int J = 0; J < N; ++J)
      A[I][J] = I + J;
}

void bar() {
  double A[10][10];

  /* foo(10, A) is inlined below */
#pragma spf assert nomacro
  {
    int N0 = 10;
    double(*A0)[N0] = A;
    for (int I = 0; I < N0; ++I)
      for (int J = 0; J < N0; ++J)
        A0[I][J] = I + J;
  }
}
