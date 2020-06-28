int bar(int Y) { return 2 * Y; }

int foo(int X) {
#pragma spf metadata replace(bar(X))
  return X + X;
}

int baz() { return foo(5); }
