int foo(int Z, int *A) {
#pragma spf assert nomacro
  {

    int X, Y, Q;
    X = Y = A[Q = A[1] + 1, Z];
    return Y + Y + Z;
  }
}
