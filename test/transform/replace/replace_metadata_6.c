struct STy { int X; };

int bar(struct STy *S) { return S->X; }

int foo(int Y) {
#pragma spf metadata replace(bar({.Y=Y}))
  return Y;
}

//CHECK: Error while processing replace_metadata_6.
//CHECK: replace_metadata_6.c:6:36: error: invalid replacement metadata
//CHECK: #pragma spf metadata replace(bar({.Y=Y}))
//CHECK:                                    ^
//CHECK: <scratch space>:5:1: note: expanded from here
//CHECK: "Y"
//CHECK: ^
//CHECK: replace_metadata_6.c:1:8: error: record has no member 'Y'
//CHECK: struct STy { int X; };
//CHECK:        ^
//CHECK: replace_metadata_6.c:6:30: error: missing replacement metadata
//CHECK: #pragma spf metadata replace(bar({.Y=Y}))
//CHECK:                              ^
//CHECK: replace_metadata_6.c:5:13: note: missing parameter in replacement metadata
//CHECK: int foo(int Y) {
//CHECK:             ^
//CHECK: 3 errors generated.
//CHECK: Error while processing replace_metadata_6.c.
