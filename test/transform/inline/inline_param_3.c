void foo(int N, double (*A)[N]) {
  for (int I = 0; I < N; ++I)
    for (int J = 0; J < N; ++J)
      A[I][J] = I + J;
}

void bar() {
  double A[10][10];
  #pragma spf transform inline
  foo(10, A);
}
//CHECK: 
