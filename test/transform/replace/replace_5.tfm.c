struct STy {
  int X;
};
struct UnusedTy {};

void foo(struct STy *S, struct UnusedTy *U) { S->X = 5; }

/* Replacement for void foo(struct STy *S, struct UnusedTy *U) */
void foo_spf0(int *S_X0) {
#pragma spf metadata replace(foo({.X = S_X0}, {}))
  (*S_X0) = 5;
}
