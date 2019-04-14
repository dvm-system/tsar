enum logical { false, true };

enum logical f(enum logical L) { return L; }

int main() {

  /* f(false) is inlined below */
  enum logical R0;
#pragma spf assert nomacro
  {
    enum logical L0 = false;
    R0 = L0;
  }
  return R0;
}
