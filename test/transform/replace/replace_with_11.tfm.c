
struct DataTy {
  float X;
};
struct ArrayTy {
  struct DataTy *Data;
  int Size;
};

void bar(struct DataTy *, int);

void baz(float *X) {
#pragma spf metadata replace(bar({.X = X}, {}))
  *X = *X * 2.0;
}

void foo_old(struct ArrayTy *);

void foo_new(struct ArrayTy *S) {
#pragma spf metadata replace(foo_old(S))
  for (int I = 0; I < S->Size; ++I) {

    bar(S->Data + I, I);
  }
}

/* Replacement for void foo_new(struct ArrayTy *S) */
void foo_new_spf0(int S_Size0, struct DataTy *S_Data0) {
#pragma spf metadata replace(foo_new({.Size = S_Size0, .Data = S_Data0}))
  for (int I = 0; I < S_Size0; ++I) {
    baz(&(S_Data0 + I)->X);
  }
}
