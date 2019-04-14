int f() { return 1; }

int main() {

  {
    if (1)
    /* f() is inlined below */
    {
      int R0;
#pragma spf assert nomacro
      { R0 = 1; }
      R0;
    }
    return 1;
  }
}
