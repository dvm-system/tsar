int bar(int Y, int Z) { return Y + Z; }

int foo(int X) {
#pragma spf metadata replace(bar(X))
  return X + X;
}

void baz() {
  #pragma spf transform replace with(foo)
  bar(5, 5);
}
//CHECK: Error while processing replace_metadata_2.
//CHECK: replace_metadata_2.c:4:36: error: expected replacement for target parameter
//CHECK: #pragma spf metadata replace(bar(X))
//CHECK:                                    ^
//CHECK: replace_metadata_2.c:1:20: note: missing parameter in replacement metadata
//CHECK: int bar(int Y, int Z) { return Y + Z; }
//CHECK:                    ^
//CHECK: 1 error generated.
//CHECK: Error while processing replace_metadata_2.c.
