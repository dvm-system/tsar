int foo(int Z) {
#pragma spf transform propagate
  int X = Z;
  {
    int Z = 5;
    return X;
  }
}
//CHECK: 
