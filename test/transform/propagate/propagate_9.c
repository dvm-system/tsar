int A[10];

void foo(int I) {
#pragma spf transform propagate
  int X = I;
  A[X = X + 1, X + 1] = I;
}
//CHECK: propagate_9.c:6:16: warning: disable expression propagation
//CHECK:   A[X = X + 1, X + 1] = I;
//CHECK:                ^
//CHECK: propagate_9.c:6:11: note: recurrence prevents propagation
//CHECK:   A[X = X + 1, X + 1] = I;
//CHECK:           ^
//CHECK: 1 warning generated.
