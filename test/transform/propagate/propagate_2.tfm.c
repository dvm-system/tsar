int foo(int X) {
  int Y;

#pragma spf assert nomacro
  {
    if (X < 0)
      Y = X;
    else
      Y = X;
    return X + X;
  }
}
