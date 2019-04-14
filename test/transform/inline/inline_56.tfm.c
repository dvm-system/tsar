int X = 9;
void foo() { X++; }
void foo_2() {
  int X = 0;

  X++;

  foo();
}
int main() {

  /* foo_2() is inlined below */
#pragma spf assert nomacro
  {
    int X = 0;

    X++;
    foo();
  }

  return 0;
}
