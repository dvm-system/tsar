int foo(int X, int Z) {
#pragma spf assert nomacro
  {

    int Y = X;
    int Q = Z;
    Y = Z;
    return Z;
  }
}
