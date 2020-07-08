struct STy {
  int X;
};

void bar(int &);

void foo(struct STy *S) { bar(S->X); }

/* Replacement for void foo(struct STy *S) */
void foo_spf0(int *S_X0) {
#pragma spf metadata replace(foo({.X = S_X0}))
  bar((*S_X0));
}
