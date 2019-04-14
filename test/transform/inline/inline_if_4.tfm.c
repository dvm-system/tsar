int f() { return 5; }

int main() {

  {
    int X;
    if (1)
    /* f() is inlined below */
    {
      int R0;
#pragma spf assert nomacro
      { R0 = 5; }
      X = R0;
    }
    return X;
  }
}
