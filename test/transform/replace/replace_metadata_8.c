void bar(double Y);

void foo(int X) {
#pragma spf metadata replace(bar(X))
}

//CHECK: Error while processing replace_metadata_8.
//CHECK: replace_metadata_8.c:4:34: error: type of target parameter and replacement parameter are not compatible
//CHECK: #pragma spf metadata replace(bar(X))
//CHECK:                                  ^
//CHECK: replace_metadata_8.c:1:17: note: declared here
//CHECK: void bar(double Y);
//CHECK:                 ^
//CHECK: replace_metadata_8.c:3:14: note: declared here
//CHECK: void foo(int X) {
//CHECK:              ^
//CHECK: replace_metadata_8.c:4:30: error: missing replacement metadata
//CHECK: #pragma spf metadata replace(bar(X))
//CHECK:                              ^
//CHECK: replace_metadata_8.c:3:14: note: missing parameter in replacement metadata
//CHECK: void foo(int X) {
//CHECK:              ^
//CHECK: 2 errors generated.
//CHECK: Error while processing replace_metadata_8.c.
