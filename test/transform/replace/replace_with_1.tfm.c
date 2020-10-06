enum Kind { K1, K2 };

typedef float T;

struct STy {
  struct QTy {
    T V1;
    T V2;
  } * Q;
  unsigned N;
  enum Kind K;
};

void foo(struct STy *S) {
  for (int I = 0; I < S->N; ++I)
    if (S->K == K1) {
      S->Q[I].V1 = S->Q[I].V2 * S->N;
    }
}

/* Replacement for void foo(struct STy *S) */
void foo_spf0(unsigned int S_N0, enum Kind S_K0, struct QTy *S_Q0) {
#pragma spf metadata replace(foo({.N = S_N0, .K = S_K0, .Q = S_Q0}))
  for (int I = 0; I < S_N0; ++I)
    if (S_K0 == K1) {
      S_Q0[I].V1 = S_Q0[I].V2 * S_N0;
    }
}

void bar(struct STy *S) { foo_spf0((S)->N, (S)->K, (S)->Q); }
