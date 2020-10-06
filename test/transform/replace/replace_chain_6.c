struct STy {
  float *X;
  int N;
};

void baz(struct STy *S);

void foo(struct STy *S, int I) {
  if (I < S->N)
    foo(S, I + 1);
  S->X[I] = I;
}           

void bar(struct STy *S1, struct STy *S2) {
#pragma spf transform replace(S1) nostrict
  foo(S1, 0);
  foo(S2, 0);
  baz(S1);
#pragma spf transform replace(S2)
}
//CHECK: replace_chain_6.c:14:10: warning: disable structure replacement
//CHECK: void bar(struct STy *S1, struct STy *S2) {
//CHECK:          ^
//CHECK: replace_chain_6.c:18:7: note: not-arrow access prevent replacement
//CHECK:   baz(S1);
//CHECK:       ^
//CHECK: 1 warning generated.
