int foo() { return 25; }

int main() {
  int x;

  x = (2 < 3) ? foo() : 0;

  return 0;
}
