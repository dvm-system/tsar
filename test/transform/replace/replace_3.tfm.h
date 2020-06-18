void foo(struct STy *S) {
#pragma spf transform replace(S)
  S->X = S->X + 1;
}

/* Replacement for void foo(struct STy *S) */
void foo_spf0(DataTy *S_X0) {
#pragma spf transform replace(S)
  (*S_X0) = (*S_X0) + 1;
}
