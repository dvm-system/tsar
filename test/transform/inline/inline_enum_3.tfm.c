typedef enum { false, true } logical;

void f(logical *L);

void f1(logical *L) {}

int main() {
  logical L;

  /* f(&L) is inlined below */
#pragma spf assert nomacro
  {
    logical *L0 = &L;
    *L0 = false;
  }

  return 0;
}
