void foo(int N, double (*A)[N/2*2+2]) {
  double (*B)[N/2*2+2] = A;
  for (int I = 0; I < N/2*2+2; ++I)
    for (int J = 0; J < N/2*2+2; ++J)
      B[I][J] = I + J;
}
