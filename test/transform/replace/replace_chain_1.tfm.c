struct STy {
  float X, Y;
};

void bar(struct STy *S) { S->Y = S->X * S->X; }

/* Replacement for void bar(struct STy *S) */
void bar_spf0(float *S_Y0, float S_X0) {
#pragma spf metadata replace(bar({.Y = S_Y0, .X = S_X0}))
  (*S_Y0) = S_X0 * S_X0;
}

void foo(struct STy *S) {
  S->X = 10;
  bar(S);
}

/* Replacement for void foo(struct STy *S) */
void foo_spf0(float *S_X1, float *S_Y1) {
#pragma spf metadata replace(foo({.X = S_X1, .Y = S_Y1}))
  (*S_X1) = 10;
  bar_spf0(S_Y1, *S_X1);
}
