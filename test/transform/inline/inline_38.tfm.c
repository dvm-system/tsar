int foo(int a) { return a - 1; }

int main() {
  int i, j = 0;

  for (i = 100; foo(i) > 0; i--) {
    j++;
  }

  return 0;
}
