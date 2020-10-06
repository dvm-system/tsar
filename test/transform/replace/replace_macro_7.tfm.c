struct STy {
  int X;
};

#define CALL bar(S);

void bar(struct STy *S) { S->X = S->X + 1; }

/* Replacement for void bar(struct STy *S) */
void bar_spf0(int *S_X0) {
#pragma spf metadata replace(bar({.X = S_X0}))
  (*S_X0) = (*S_X0) + 1;
}

int foo(struct STy *S) {
#pragma spf transform replace(S) nostrict
  CALL return S->X;
}
