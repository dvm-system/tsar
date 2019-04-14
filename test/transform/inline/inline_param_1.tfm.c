void f(int *A, int I) { A[I] = 10; }

void g() {
  int A[10];

  /* f(A, 1) is inlined below */
#pragma spf assert nomacro
  {
    int *A0 = A;
    int I0 = 1;
    A0[I0] = 10;
  }
}
