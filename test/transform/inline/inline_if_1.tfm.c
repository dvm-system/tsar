int f() { return 1; }

int main() {

  {
    /* f() is inlined below */
    int R0;
#pragma spf assert nomacro
    { R0 = 1; }
    if (R0)
      return 0;
    return 1;
  }
}
