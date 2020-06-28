#define T int
struct STy {
  T X;
};

#define STy struct STy
int foo(STy *S, T X) { return S->X + X; }

/* Replacement for int foo(STy *S, T X) */
int foo_spf0(int S_X0, T X) {
#pragma spf metadata replace(foo({.X = S_X0}, X))
  return S_X0 + X;
}
