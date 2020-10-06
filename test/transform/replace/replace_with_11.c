
struct DataTy { float X; };
struct ArrayTy { struct DataTy *Data; int Size; };

void bar(struct DataTy *, int);

void baz(float *X) {
  #pragma spf metadata replace(bar({.X=X},{}))
  *X = *X * 2.0;
}

void foo_old(struct ArrayTy *); 

void foo_new(struct ArrayTy *S) {
#pragma spf metadata replace(foo_old(S)) 
  for (int I = 0; I < S->Size; ++I) {
#pragma spf transform replace(S) with(baz) nostrict
    bar(S->Data + I, I);
  }
}

//CHECK: 
