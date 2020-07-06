#define SIZE 4

typedef float DataTy;

struct STy {
  DataTy T[SIZE];
};

void foo(struct STy *S, DataTy Val) { S->T[0] = Val; }

/* Replacement for void foo(struct STy *S) */
void foo_spf0(DataTy *S_T0, DataTy Val) {
#pragma spf metadata replace(foo({.T = S_T0}, Val))
  S_T0[0] = Val;
}

void bar(struct STy *S, DataTy Val) {
  #pragma spf transform replace(S) with(foo_spf0) nostrict
  foo(S, Val);
}
//CHECK: 
