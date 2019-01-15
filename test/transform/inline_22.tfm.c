void foo(int *a) { *a = 900; }

int main() {
  int a = 0;

  /* foo(&a) is inlined below */
#pragma spf assert nomacro
  {
    int *a0 = &a;
    *a0 = 900;
  }

  return 0;
}
