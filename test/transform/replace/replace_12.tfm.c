struct STy {
  int X;
};
struct UnusedTy {};

void foo(struct STy *S, struct UnusedTy *U) { S->X = 5; }

/* Replacement for void foo(struct STy *S, struct UnusedTy *U) */
void foo_spf0(struct STy *S) {
#pragma spf metadata replace(foo(S, {}))
  S->X = 5;
}
