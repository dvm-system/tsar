struct STy {
  int X;
};
struct QTy {
  struct STy P;
};

int foo(struct QTy S) {
  int Y = S.P.X;

#pragma spf assert nomacro
  { return S.P.X; }
}
