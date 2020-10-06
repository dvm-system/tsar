int bar(int Y, int Z) { return Y + Z; }

int foo(int X) {
#pragma spf metadata replace(bar(X, X))
  return X + X;
}

void baz() { foo(5); }
