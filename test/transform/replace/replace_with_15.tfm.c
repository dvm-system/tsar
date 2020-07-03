struct STy {
  float *V;
};

void foo(struct STy *S) { S->V[2] = 2; }

/* Replacement for void foo(struct STy *S) */
void foo_spf0(float **S_V0) {
#pragma spf metadata replace(foo({.V = S_V0}))
  (*S_V0)[2] = 2;
}

void bar(struct STy *S) { foo(S); }

/* Replacement for void bar(struct STy *S) */
void bar_spf0(float **S_V1) {
#pragma spf metadata replace(bar({.V = S_V1}))
  foo_spf0(S_V1);
}
