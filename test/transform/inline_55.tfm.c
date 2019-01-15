int X = 9;

void foo() { X++; }

int main() {

  /* foo() is inlined below */
#pragma spf assert nomacro
  { X++; }

  return 0;
}
