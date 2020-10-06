int bar(int Y, int Z) { return Y + Z; }

int foo(int X) {
#pragma spf metadata replace(bar(X, X))
  return X + X;
}

void baz() {
  #pragma spf transform replace with(foo)
  bar(5, 5);
}
//CHECK: Error while processing replace_metadata_1.
//CHECK: replace_metadata_1.c:4:37: error: replacement metadata for parameter already set
//CHECK: #pragma spf metadata replace(bar(X, X))
//CHECK:                                     ^
//CHECK: 1 error generated.
//CHECK: Error while processing replace_metadata_1.c.
