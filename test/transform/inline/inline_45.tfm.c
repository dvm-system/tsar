int foo() { return 95; }

int main() {
  int x = 56;

  if (x > 100)
    x += foo();
  else
    x -= foo();

  return 0;
}
