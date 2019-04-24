int bar();

int foo(int Z) {
#pragma spf transform propagate
  int X;
  {
    int Y = bar();
    X = Y;
  }
  return X;
}
//CHECK: 
