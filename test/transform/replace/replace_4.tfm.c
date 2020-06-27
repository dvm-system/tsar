struct UnusedTy {};

void foo(struct UnusedTy *U1, struct UnusedTy *U2) {}

/* Replacement for void foo(struct UnusedTy *U1, struct UnusedTy *U2) */
void foo_spf0() {
#pragma spf metadata replace(foo({}, {}))
}
