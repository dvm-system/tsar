
struct STy;
void foo(struct STy *S, int I);

void baz(struct STy *S, int I) { foo(S, I); }

typedef float DataT;

struct STy {
  DataT *X;
  int N;
};

void foo(struct STy *S, int I) {
  if (I < S->N)
    foo(S, I + 1);
  S->X[I] = I;
}

/* Replacement for void foo(struct STy *S, int I) */
void foo_spf0(int S_N0, DataT *S_X0, int I) {
#pragma spf metadata replace(foo({.N = S_N0, .X = S_X0}, I))
  if (I < S_N0)
    foo_spf0(S_N0, S_X0, I + 1);
  S_X0[I] = I;
}

void bar(struct STy *S1, struct STy *S2) {

  baz(S1, 0);
  S2->N = 10;
}

/* Replacement for void bar(struct STy *S1, struct STy *S2) */
void bar_spf0(struct STy *S1, int *S2_N0) {
#pragma spf metadata replace(bar(S1, {.N = S2_N0}))
  baz(S1, 0);
  (*S2_N0) = 10;
}
