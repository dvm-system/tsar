int bar();

int foo(int Z) {

  int X;
  {
    int Y = bar();
    X = Y;
  }
  return X;
}
