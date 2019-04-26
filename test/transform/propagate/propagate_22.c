int foo(int Z, int *A) {
#pragma spf transform propagate
  int X, Y, Q;
  X = Y = A[Q = A[1] + 1, Z];
  return X + Y + Z;
}
//CHECK: propagate_22.c:5:14: warning: disable expression propagation
//CHECK:   return X + Y + Z;
//CHECK:              ^
//CHECK: propagate_22.c:4:11: note: expression is not available at propagation point
//CHECK:   X = Y = A[Q = A[1] + 1, Z];
//CHECK:           ^
//CHECK: propagate_22.c:3:13: note: value may differs in definition and propagation points or may produce side effect
//CHECK:   int X, Y, Q;
//CHECK:             ^
//CHECK: 1 warning generated.
