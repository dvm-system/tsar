int X = 9;

void foo(int X) { X++; }

int main() {

  /* foo(71) is inlined below */
#pragma spf assert nomacro
  {
    int X0 = 71;
    X0++;
  }

  return 0;
}
