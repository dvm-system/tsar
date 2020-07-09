struct STy { float X[4]; };

void foo(struct STy *S) {
#pragma spf transform replace(S)
  S->X[3] = 10;
}
//CHECK: 
