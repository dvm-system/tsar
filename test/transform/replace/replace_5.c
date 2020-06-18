struct STy { int X; };
struct UnusedTy {};

void foo(struct STy *S, struct UnusedTy *U) {
#pragma spf transform replace(S, U)
  S->X = 5;
}
//CHECK: replace_5.c:4:42: remark: structure replacement
//CHECK: void foo(struct STy *S, struct UnusedTy *U) {
//CHECK:                                          ^
//CHECK: replace_5.c:4:25: remark: remove unused declaration
//CHECK: void foo(struct STy *S, struct UnusedTy *U) {
//CHECK:                         ^
