struct STy {
  float *X;
  int N;
};

void baz(struct STy *S);

void foo(struct STy *S) { baz(S); }

void bar(struct STy *S) {
#pragma spf transform replace(S)
  foo(S);
}
//CHECK: replace_chain_8.c:8:10: warning: disable structure replacement
//CHECK: void foo(struct STy *S) { baz(S); }
//CHECK:          ^
//CHECK: replace_chain_8.c:8:31: note: not-arrow access prevent replacement
//CHECK: void foo(struct STy *S) { baz(S); }
//CHECK:                               ^
//CHECK: replace_chain_8.c:10:22: warning: disable structure replacement
//CHECK: void bar(struct STy *S) {
//CHECK:                      ^
//CHECK: replace_chain_8.c:12:7: note: not-arrow access prevent replacement
//CHECK:   foo(S);
//CHECK:       ^
//CHECK: 2 warnings generated.
