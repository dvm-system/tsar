int f() { return 3; }

void g() { 0 ? f() : 4; }

void h() {

  /* g() is inlined below */
#pragma spf assert nomacro
  { 0 ? f() : 4; }
}
