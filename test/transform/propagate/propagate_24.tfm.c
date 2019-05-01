int bar();

int foo(int Z) {
#pragma spf assert nomacro
  {

    int X;
    {
      int Y = bar();
      X = Y;
    }
    return X;
  }
}
