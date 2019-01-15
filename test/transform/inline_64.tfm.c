int foo(int a) { return a--; }

int main() {
  int x = 50;

  while (foo(x) > 0) {

    x -= 1;
  }

  return 0;
}
