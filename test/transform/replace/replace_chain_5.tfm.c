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
void foo_spf1(int *S_N1, float **S_X1, int I) {
#pragma spf metadata replace(foo({.N = S_N1, .X = S_X1}, I))
  if (I < (*S_N1))
    foo_spf1(S_N1, S_X1, I + 1);

  (*S_X1)[I] = I;
}

/* Replacement for void foo(struct STy *S, int I) */
void foo_spf0(int S_N0, float *S_X0, int I) {
#pragma spf metadata replace(foo({.N = S_N0, .X = S_X0}, I))
  if (I < S_N0)
    foo_spf0(S_N0, S_X0, I + 1);
  S_X0[I] = I;
}

void bar(struct STy *S) {
  foo(S, 0);

  foo(S, 1);
}

/* Replacement for void bar(struct STy *S) */
void bar_spf0(int *S_N2, float **S_X2) {
#pragma spf metadata replace(bar({.N = S_N2, .X = S_X2}))
  foo_spf1(S_N2, S_X2, 0);
  foo_spf0(*S_N2, *S_X2, 1);
}
