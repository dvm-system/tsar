struct STy { int X; };
struct UnusedTy {};

void foo(struct STy *S, struct UnusedTy *U) {
#pragma spf transform replace(U)
  S->X = 5;
}
//CHECK: replace_12.c:4:42: remark: structure replacement
//CHECK: void foo(struct STy *S, struct UnusedTy *U) {
//CHECK:                                          ^
//CHECK: replace_12.c:4:25: remark: remove unused declaration
//CHECK: void foo(struct STy *S, struct UnusedTy *U) {
//CHECK:                         ^
