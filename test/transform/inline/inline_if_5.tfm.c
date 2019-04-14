int f() { return 1; }

int g(int X) {

  if (X && f())
    return X;
  return 0;
}
