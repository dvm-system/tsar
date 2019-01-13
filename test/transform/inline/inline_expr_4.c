int foo() { return 5; }
int bar(int X) { return X; }

int main() {
  #pragma spf transform inline
  int X = bar(foo()) + foo();

  return 0;
}
