int X;

void f() { X = 4; }

void g() {

  /* f() is inlined below */
#pragma spf assert nomacro
  { X = 4; }
}

int h() {
  int X = 5;

  /* g() is inlined below */
#pragma spf assert nomacro
  { f(); }

  return X;
}
