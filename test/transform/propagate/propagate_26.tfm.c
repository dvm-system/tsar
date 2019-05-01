int foo(int Z) {
#pragma spf assert nomacro
  {

    int X = Z;
    for (int Z = 5; Z < X; ++Z) {
      if (Z == X - 1)
        return X;
    }
    return Z + 1;
  }
}
