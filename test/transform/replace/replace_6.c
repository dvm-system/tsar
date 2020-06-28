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
  #pragma spf transform replace(S) nostrict
}
//CHECK: 
