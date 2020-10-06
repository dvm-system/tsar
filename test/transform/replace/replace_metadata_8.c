void bar(double (*Y)[10]);

void foo(double X) {
#pragma spf metadata replace(bar(X))
}

//CHECK: Error while processing replace_metadata_8.
//CHECK: replace_metadata_8.c:4:34: error: passing 'double (*)[10]' to parameter of incompatible type 'double'
//CHECK: #pragma spf metadata replace(bar(X))
//CHECK:                                  ^
//CHECK: replace_metadata_8.c:4:34: error: type of target parameter and replacement parameter are not compatible
//CHECK: replace_metadata_8.c:1:19: note: declared here
//CHECK: void bar(double (*Y)[10]);
//CHECK:                   ^
//CHECK: replace_metadata_8.c:3:17: note: declared here
//CHECK: void foo(double X) {
//CHECK:                 ^
//CHECK: replace_metadata_8.c:4:30: error: missing replacement metadata
//CHECK: #pragma spf metadata replace(bar(X))
//CHECK:                              ^
//CHECK: replace_metadata_8.c:3:17: note: missing parameter in replacement metadata
//CHECK: void foo(double X) {
//CHECK:                 ^
//CHECK: 3 errors generated.
//CHECK: Error while processing replace_metadata_8.c.
