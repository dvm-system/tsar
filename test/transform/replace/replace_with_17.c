#define SIZE 4

typedef float DataTy;

struct STy {
  DataTy T[SIZE];
};

void foo(struct STy *S) { S->T[2] = 2; }

/* Replacement for void foo(struct STy *S) */
void foo_spf0(DataTy *S_T0) {
#pragma spf metadata replace(foo({.T = S_T0}))
  S_T0[2] = 2;
}

void bar(struct STy *S) {
  #pragma spf transform replace with(foo_spf0)
  foo(S);
}
//CHECK: 
