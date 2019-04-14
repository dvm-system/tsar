typedef enum { false, true } logical;

logical f(logical L) { return L; }

int main() {

  /* f(false) is inlined below */
  logical R0;
#pragma spf assert nomacro
  {
    logical L0 = false;
    R0 = L0;
  }
  return R0;
}
