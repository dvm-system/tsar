void f() {
  if (1)
    return;
  return;
}

void g() {

  /* f() is inlined below */
#pragma spf assert nomacro
  {
    if (1)
      goto L0;
    ;
  }
L0:;
}
