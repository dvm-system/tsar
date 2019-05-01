struct STy {
  int X, Y;
};

int foo(struct STy S) {
#pragma spf assert nomacro
  {

    int Z = S.Y;
    return S.Y;
  }
}
