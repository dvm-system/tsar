int foo() {
  int X;
  int Y = (X = 5);
  return X;
}
//CHECK: de_decls_42.c:3:7: warning: disable dead code elimination
//CHECK:   int Y = (X = 5);
//CHECK:       ^
//CHECK: de_decls_42.c:3:12: warning: side effect prevent dead code elimination
//CHECK:   int Y = (X = 5);
//CHECK:            ^
//CHECK: 2 warnings generated.
