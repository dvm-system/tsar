struct pair {
  int a;
  int b;
};

struct pair foo(int x, int y) {
  struct pair m;
  m.a = x;
  m.b = y;
  return m;
}

int main() {
  struct pair p;

  /* foo(4, 90) is inlined below */
  struct pair R0;
#pragma spf assert nomacro
  {
    int x0 = 4;
    int y0 = 90;
    struct pair m;
    m.a = x0;
    m.b = y0;
    R0 = m;
  }
  p = R0;
  return 0;
}
