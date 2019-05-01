struct STy {
  int Y;
  int X;
};

int foo(int Y) {
#pragma spf assert nomacro
  {

    struct STy S;
    S.X = Y;
    return Y;
  }
}
