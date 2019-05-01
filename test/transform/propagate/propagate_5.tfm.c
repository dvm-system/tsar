int foo() {

#pragma spf assert nomacro
  {
    int A[3], X, Y;
    unsigned Z;
    A[0] = A[1] = A[2] = 1;
    X = A[0];
    Y = A[1];
    Z = A[2];
    return 1 + 1 + 1;
  }
}
