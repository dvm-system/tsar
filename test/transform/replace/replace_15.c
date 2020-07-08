struct STy { int X; };

void bar(int *);

void foo(struct STy *S) {
#pragma spf transform replace(S) nostrict
  bar (&S->X);
}
//CHECK: 
