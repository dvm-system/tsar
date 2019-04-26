int foo(int Z) {
#pragma spf transform propagate
  int X = Z;
  for (int Z = 5; Z < X; ++Z) {
    if (Z == X - 1)
      return X;
  }
  return X + 1;
}
//CHECK: propagate_26.c:4:23: warning: disable expression propagation
//CHECK:   for (int Z = 5; Z < X; ++Z) {
//CHECK:                       ^
//CHECK: propagate_26.c:1:13: note: hidden dependence prevents propagation
//CHECK: int foo(int Z) {
//CHECK:             ^
//CHECK: propagate_26.c:5:14: warning: disable expression propagation
//CHECK:     if (Z == X - 1)
//CHECK:              ^
//CHECK: propagate_26.c:1:13: note: hidden dependence prevents propagation
//CHECK: int foo(int Z) {
//CHECK:             ^
//CHECK: propagate_26.c:6:23: warning: disable expression propagation
//CHECK:       return X;
//CHECK:              ^
//CHECK: propagate_26.c:1:13: note: hidden dependence prevents propagation
//CHECK: int foo(int Z) {
//CHECK:             ^
//CHECK: 3 warnings generated.
