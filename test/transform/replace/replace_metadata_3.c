int bar(int Y) { return Y; }

int foo(int X, int Z) {
#pragma spf metadata replace(bar(X))
  return X + Z;
}
//CHECK: Error while processing replace_metadata_3.
//CHECK: replace_metadata_3.c:4:30: error: missing replacement metadata
//CHECK: #pragma spf metadata replace(bar(X))
//CHECK:                              ^
//CHECK: replace_metadata_3.c:3:20: note: missing parameter in replacement metadata
//CHECK: int foo(int X, int Z) {
//CHECK:                    ^
//CHECK: 1 error generated.
//CHECK: Error while processing replace_metadata_3.c.
