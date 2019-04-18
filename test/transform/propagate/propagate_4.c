struct STy {
  int X, Y;
};

int foo(struct STy S) {
#pragma spf transform propagate
  int Z = S.Y;
  return Z;
}
//CHECK: 
