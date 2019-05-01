int foo(int Z) {
#pragma spf assert nomacro
  {

    int X = Z;
    {
      int Z = 5;
      return X;
    }
  }
}
