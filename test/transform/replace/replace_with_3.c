int bar(int Y) { return 2 * Y; }

int foo(int X) {
#pragma spf metadata replace(bar(X))
  return X + X;
}

int baz() {
#pragma spf transform replace with(foo)
  return bar(5);
}
//CHECK: 
