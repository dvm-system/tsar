int A[10];

void foo(int I) {
#pragma spf assert nomacro
  {

    int X = I;
    A[X = I + 1, X + 1] = I;
  }
}
