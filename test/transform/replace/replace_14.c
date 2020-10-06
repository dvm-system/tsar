struct STy { int *X; };

int * foo(struct STy *S) {
#pragma spf transform replace(S) nostrict
  return &(S->X[4]);
}
//CHECK: 
