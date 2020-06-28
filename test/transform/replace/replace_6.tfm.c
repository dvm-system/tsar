enum Kind { K1, K2 };

typedef float T;

struct STy {
  struct QTy {
    T V1;
    T V2;
  } * Q;
  unsigned N;
  enum Kind K;
  T R;
  T (*F)(T, T);
};

void foo(struct STy *S) {
  for (int I = 0; I < S->N; ++I)
    if (S->K == K1)
      S->R = S->F(S->R, S->Q[I].V1);
    else
      S->R = S->F(S->R, S->Q[I].V2);
}

/* Replacement for void foo(struct STy *S) */
void foo_spf0(unsigned int S_N0, enum Kind S_K0, T *S_R0, T (*S_F0)(T, T),
              struct QTy *S_Q0) {
#pragma spf metadata replace(                                                  \
    foo({.N = S_N0, .K = S_K0, .R = S_R0, .F = S_F0, .Q = S_Q0}))
  for (int I = 0; I < S_N0; ++I)
    if (S_K0 == K1)
      (*S_R0) = S_F0((*S_R0), S_Q0[I].V1);
    else
      (*S_R0) = S_F0((*S_R0), S_Q0[I].V2);
}
