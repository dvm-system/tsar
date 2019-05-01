int foo(int X) {
#pragma spf assert nomacro
  {

    int Y = X;
    return X;
  }
}
