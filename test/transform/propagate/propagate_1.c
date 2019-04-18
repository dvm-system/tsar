int foo(int X) {
#pragma spf transform propagate
  int Y = X;
  return Y;
}
//CHECK: 
