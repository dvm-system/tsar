struct UnusedTy {};

void foo(struct UnusedTy *U1, struct UnusedTy *U2) {
#pragma spf transform replace(U1, U2)
}
//CHECK: replace_4.c:3:48: remark: structure replacement
//CHECK: void foo(struct UnusedTy *U1, struct UnusedTy *U2) {
//CHECK:                                                ^
//CHECK: replace_4.c:3:31: remark: remove unused declaration
//CHECK: void foo(struct UnusedTy *U1, struct UnusedTy *U2) {
//CHECK:                               ^
//CHECK: replace_4.c:3:27: remark: structure replacement
//CHECK: void foo(struct UnusedTy *U1, struct UnusedTy *U2) {
//CHECK:                           ^
//CHECK: replace_4.c:3:10: remark: remove unused declaration
//CHECK: void foo(struct UnusedTy *U1, struct UnusedTy *U2) {
//CHECK:          ^
