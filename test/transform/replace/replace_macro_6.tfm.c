struct STy {
  int X;
};

#define _S_ S

void bar(struct STy *S) { S->X = S->X + 1; }

/* Replacement for void bar(struct STy *S) */
void bar_spf0(int *S_X0) {
#pragma spf metadata replace(bar({.X = S_X0}))
  (*S_X0) = (*S_X0) + 1;
}

int foo(struct STy *S) {

  bar(_S_);
  return _S_->X;
}

/* Replacement for int foo (struct STy *S) */
int foo_spf0(int *S_X1) {
#pragma spf metadata replace(foo({.X = S_X1}))
  bar_spf0(S_X1);
  return (*S_X1);
}
