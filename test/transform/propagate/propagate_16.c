struct STy { int X; };
struct QTy { struct STy P; };

int foo(struct QTy S) {
  int Y = S.P.X;
  #pragma spf transform propagate
  {
    return Y;
  }
}
//CHECK: 
