int foo(int Z) {
#pragma spf transform propagate
  int X = Z;
  {
    int Z = 5;
    return X;
  }
}
//CHECK: propagate_25.c:6:12: warning: disable expression propagation
//CHECK:     return X;
//CHECK:            ^
//CHECK: propagate_25.c:1:13: note: hidden dependence prevents propagation
//CHECK: int foo(int Z) {
//CHECK:             ^
//CHECK: 1 warning generated.
