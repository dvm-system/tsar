struct STy { int X; };

int bar(struct STy *S) { return S->X; }

int foo(int X, int Y) {
#pragma spf metadata replace(bar({.X=X}, Y))
  return X;
}

int baz() {
  struct STy S;
  #pragma spf transform replace with(foo)
  bar(&S);
  return S.X;
}
//CHECK: Error while processing replace_metadata_5.
//CHECK: replace_metadata_5.c:6:30: error: function does not take 2 arguments
//CHECK: #pragma spf metadata replace(bar({.X=X}, Y))
//CHECK:                              ^
//CHECK: replace_metadata_5.c:3:5: note: declared here
//CHECK: int bar(struct STy *S) { return S->X; }
//CHECK:     ^
//CHECK: replace_metadata_5.c:6:30: error: missing replacement metadata
//CHECK: #pragma spf metadata replace(bar({.X=X}, Y))
//CHECK:                              ^
//CHECK: replace_metadata_5.c:5:20: note: missing parameter in replacement metadata
//CHECK: int foo(int X, int Y) {
//CHECK:                    ^
//CHECK: 2 errors generated.
//CHECK: Error while processing replace_metadata_5.c.
