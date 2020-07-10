
struct STy;
void foo(struct STy *S, int I);

void baz(struct STy *S, int I) {
  foo(S, I);
}

typedef float DataT;

struct STy {
  DataT *X;
  int N;
};

void foo(struct STy *S, int I) {
  if (I < S->N)
    foo(S, I + 1);
  S->X[I] = I;
}

void bar(struct STy *S1, struct STy *S2) {
#pragma spf transform replace(S1, S2) nostrict
  baz(S1, 0);
  S2->N = 10;
}
//CHECK: replace_chain_9.c:5:22: warning: disable structure replacement
//CHECK: void baz(struct STy *S, int I) {
//CHECK:                      ^
//CHECK: replace_chain_9.c:12:3: note: unable to build declaration of a record member
//CHECK:   DataT *X;
//CHECK:   ^
//CHECK: replace_chain_9.c:22:22: warning: disable structure replacement
//CHECK: void bar(struct STy *S1, struct STy *S2) {
//CHECK:                      ^
//CHECK: replace_chain_9.c:24:7: note: not-arrow access prevent replacement
//CHECK:   baz(S1, 0);
//CHECK:       ^
//CHECK: 2 warnings generated.
