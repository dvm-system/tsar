struct STy {
  int X;
};

#define STyS struct STy *S

int foo(STyS, int X) { return S->X = X; }

/* Replacement for int foo(STyS, int X) */
int foo_spf0(int *S_X0, int X) {
#pragma spf metadata replace(foo({.X = S_X0}, X))
  return (*S_X0) = X;
}
