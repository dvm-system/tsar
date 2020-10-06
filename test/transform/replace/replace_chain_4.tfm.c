struct STy {
  float *X;
  int N;
};

void foo(struct STy *S, int I) {
  if (I < S->N)
    foo(S, I + 1);

  S->X[I] = I;
}

/* Replacement for void foo(struct STy *S, int I) */
void foo_spf0(int S_N0, float *S_X0, int I) {
#pragma spf metadata replace(foo({.N = S_N0, .X = S_X0}, I))
  if (I < S_N0)
    foo_spf0(S_N0, S_X0, I + 1);
  S_X0[I] = I;
}
