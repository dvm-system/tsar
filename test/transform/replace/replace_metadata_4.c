struct STy { int X; };

int bar(struct STy *S) { return S->X; }

int foo(int X) {
#pragma spf metadata replace(bar({.=X}))
  return X;
}
//CHECK: Error while processing replace_metadata_4.
//CHECK: replace_metadata_4.c:6:34: error: expected ')'
//CHECK: #pragma spf metadata replace(bar({.=X}))
//CHECK:                                  ^
//CHECK: replace_metadata_4.c:6:35: error: expected name of clause
//CHECK: #pragma spf metadata replace(bar({.=X}))
//CHECK:                                   ^
//CHECK: 2 errors generated.
//CHECK: Error while processing replace_metadata_4.c.
