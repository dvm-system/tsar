struct STy { int X; };

#define _S_ S

void bar(struct STy *S) {
  S->X = S->X + 1;
}

int foo (struct STy *S) {
 #pragma spf transform replace(S) nostrict
 bar(_S_);
 return _S_->X;
}

//CHECK: 
