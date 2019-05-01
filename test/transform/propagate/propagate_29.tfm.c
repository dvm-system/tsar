int foo(int Y) {
  int A[1] = {0};

#pragma spf assert nomacro
  {
    int Z;
    if (Y > 0)
      A[0] = Y;
    Z = A[0];
    return Y + A[0];
  }
}
