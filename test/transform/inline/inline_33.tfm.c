int foo() { return 25; }

int main() {
  int x;

  x = (2 > 3) ? 0 : foo();

  return 0;
}
