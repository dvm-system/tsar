void bar(int Y);

void foo(int X) {
  int Y;
#pragma spf metadata replace(bar(Y))
}

//CHECK: Error while processing replace_metadata_7.
//CHECK: replace_metadata_7.c:5:34: error: expected function parameter name
//CHECK: #pragma spf metadata replace(bar(Y))
//CHECK:                                  ^
//CHECK: replace_metadata_7.c:4:7: note: declared here
//CHECK:   int Y;
//CHECK:       ^
//CHECK: replace_metadata_7.c:5:30: error: missing replacement metadata
//CHECK: #pragma spf metadata replace(bar(Y))
//CHECK:                              ^
//CHECK: replace_metadata_7.c:3:14: note: missing parameter in replacement metadata
//CHECK: void foo(int X) {
//CHECK:              ^
//CHECK: 2 errors generated.
//CHECK: Error while processing replace_metadata_7.c.
