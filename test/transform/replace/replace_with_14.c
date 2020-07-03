struct STy { float *V; };

void foo(struct STy *S) { S->V[2] = 2; }

/* Replacement for void foo(struct STy *S) */
void foo_spf0(float *S_V0) {
#pragma spf metadata replace(foo({.V = S_V0}))
  S_V0[2] = 2;
}

void bar(struct STy *S) {
  #pragma spf transform replace(S) with(foo_spf0)
  foo(S);
}
//CHECK: 
