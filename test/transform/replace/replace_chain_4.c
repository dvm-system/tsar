struct STy { float *X; int N; };

void foo(struct STy *S, int I) {
  if (I < S->N)
   foo(S, I + 1);
#pragma spf transform replace(S) nostrict
  S->X[I] = I;
}
//CHECK: 
