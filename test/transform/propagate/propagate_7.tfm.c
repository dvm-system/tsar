struct STy {
  int Y;
  int X;
};

int foo(int Y) {

  struct STy S;
  S.X = Y;
  return Y;
}
