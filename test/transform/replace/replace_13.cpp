struct UnusedTy {};

struct STy { 
  typedef int Int;
  Int X;
};

void foo(struct STy *S, struct UnusedTy *U) {
#pragma spf transform replace(S, U)
  S->X = 5;
}
//CHECK: replace_13.cpp:8:22: warning: disable structure replacement
//CHECK: void foo(struct STy *S, struct UnusedTy *U) {
//CHECK:                      ^
//CHECK: replace_13.cpp:5:3: note: unable to build declaration of a record member
//CHECK:   Int X;
//CHECK:   ^
//CHECK: replace_13.cpp:8:42: remark: structure replacement
//CHECK: void foo(struct STy *S, struct UnusedTy *U) {
//CHECK:                                          ^
//CHECK: replace_13.cpp:8:25: remark: remove unused declaration
//CHECK: void foo(struct STy *S, struct UnusedTy *U) {
//CHECK:                         ^
//CHECK: 1 warning generated.
