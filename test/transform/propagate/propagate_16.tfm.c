struct STy {
  int X;
};
struct QTy {
  struct STy P;
};

int foo(struct QTy S) {
  int Y = S.P.X;

  { return S.P.X; }
}
