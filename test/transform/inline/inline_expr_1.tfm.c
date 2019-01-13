int M(int X) { return X; }

int main() {
  int X = 0;

  {
    if (1)
    /* M(78) is inlined below */
    {
      int R0;
#pragma spf assert nomacro
      {
        int X0 = 78;
        R0 = X0;
      }
      X = R0 + X;
    }
  }
  return 0;
}
