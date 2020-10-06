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

void bar(struct STy *S, DataTy Val) { foo(S, Val); }

/* Replacement for void bar(struct STy *S, DataTy Val) */
void bar_spf0(DataTy S_T1[4], DataTy Val) {
#pragma spf metadata replace(bar({.T = S_T1}, Val))
  foo_spf0(S_T1, Val);
}
