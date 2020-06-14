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
  #pragma spf transform replace(S) nostrict
}
//CHECK: 
