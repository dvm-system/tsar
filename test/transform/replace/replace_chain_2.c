struct STy { float X, Y; };

void bar(struct STy *S) {
  S->Y = S->X * S->X;
}

void foo(struct STy *S) {
  S->X = 10;
  bar(S);
#pragma spf transform replace(S)
}
//CHECK: 
