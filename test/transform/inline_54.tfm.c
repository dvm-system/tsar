int N = 50;

void foo(int *a) {
  int i = 0;
  for (i = 0; i < N; i++)
    a[i] = 0;
}

int main() {
  int b[N];

  /* foo(b) is inlined below */
#pragma spf assert nomacro
  {
    int *a0 = b;
    int i = 0;
    for (i = 0; i < N; i++)
      a0[i] = 0;
  }

  return 0;
}
