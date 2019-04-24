int foo(int Z) {

  int X = Z;
  for (int Z = 5; Z < X; ++Z) {
    if (Z == X - 1)
      return X;
  }
  return Z + 1;
}
