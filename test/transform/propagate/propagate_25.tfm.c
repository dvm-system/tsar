int foo(int Z) {

  int X = Z;
  {
    int Z = 5;
    return X;
  }
}
