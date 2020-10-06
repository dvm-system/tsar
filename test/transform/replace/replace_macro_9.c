struct STy { int X; };

#define CALL bar

int bar(int X) { return X * 2; }
int bar_new(int X) {
#pragma spf metadata replace(bar(X))
  return X + X;
}

int foo (int X) {
 #pragma spf transform replace with(bar_new)
 return CALL(X);
}

//CHECK: 
