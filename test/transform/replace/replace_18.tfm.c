struct STy {
  float X[4];
};

void foo(struct STy *S) { S->X[3] = 10; }

/* Replacement for void foo(struct STy *S) */
void foo_spf0(float S_X0[4]) {
#pragma spf metadata replace(foo({.X = S_X0}))
  S_X0[3] = 10;
}
