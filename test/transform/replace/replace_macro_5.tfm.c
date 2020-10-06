struct STy {
  int X;
};

#define _S_ S

int foo(struct STy *S, int X) { return _S_->X = X; }

/* Replacement for int foo (struct STy *S, int X) */
int foo_spf0(int *S_X0, int X) {
#pragma spf metadata replace(foo({.X = S_X0}, X))
  return (*S_X0) = X;
}
