int f(int);

void f1() {

  /* f(5) is inlined below */
  int R0;
#pragma spf assert nomacro
  {
    int A0 = 5;
    R0 = A0;
  }
  R0;
}

int A;

int f(int A) { return A; }
