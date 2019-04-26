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
//CHECK: propagate_24.c:10:10: warning: disable expression propagation
//CHECK:   return X;
//CHECK:          ^
//CHECK: propagate_24.c:7:9: note: hidden dependence prevents propagation
//CHECK:     int Y = bar();
//CHECK:         ^
//CHECK: 1 warning generated.
