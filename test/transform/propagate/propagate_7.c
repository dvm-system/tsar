struct STy { int Y; int X; };

int foo(int Y) {
#pragma spf transform propagate
  struct STy S;
  S.X = Y;
  return S.X;
}
//CHECK: 
