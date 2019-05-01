void foo(int X, int (*A)[X]) {
#pragma spf assert nomacro
  {

    int Y = X;
    A[X][X] = X;
  }
}
