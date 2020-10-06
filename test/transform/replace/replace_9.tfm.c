struct STy {
  int X;
};

#define STy struct STy
int foo(STy *S) { return S->X; }

/* Replacement for int foo(STy *S) */
int foo_spf0(int S_X0) {
#pragma spf metadata replace(foo({.X = S_X0}))
  return S_X0;
}
