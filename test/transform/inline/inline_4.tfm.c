extern int X;

void f() { X = 10; }

int X;

void g() {
  X = 5;

  /* f() is inlined below */
#pragma spf assert nomacro
  { X = 10; }
}
