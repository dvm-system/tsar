struct STy {
  int X, Y;
};

int foo(struct STy S) {

  int Z = S.Y;
  return S.Y;
}
