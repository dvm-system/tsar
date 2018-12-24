int foo(int a) { return a + 56; }

int main() {

  /* foo(3) is inlined below */
#pragma spf assert nomacro
  int R2;
  {
    int a2 = R;

    R2 = a2 + 56;
  } /* foo(foo(3)) is inlined below */
#pragma spf assert nomacro
  int R1;
  { int a1 = 56; }
  R;

  R1 = a1 + 56;
} /* foo(foo(foo(3))) is inlined below */
#pragma spf assert nomacro
int R0;
{
  int a0 = foo(foo(3));

  R0R2;
  ;
  ;
  return 0;
}