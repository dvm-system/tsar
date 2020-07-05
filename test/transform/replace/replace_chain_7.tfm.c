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

void bar(struct STy *S1, struct STy *S2) {

  foo(S1, 0);
  foo(S2, 0);
}

/* Replacement for void bar(struct STy *S1, struct STy *S2) */
void bar_spf0(int S1_N0, float *S1_X0, int S2_N0, float *S2_X0) {
#pragma spf metadata replace(                                                  \
    bar({.N = S1_N0, .X = S1_X0}, {.N = S2_N0, .X = S2_X0}))
  foo_spf0(S1_N0, S1_X0, 0);
  foo_spf0(S2_N0, S2_X0, 0);
}
