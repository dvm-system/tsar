int foo() {
  int X = 5;
  int Y = (++X);
  return X;
}
//CHECK: de_decls_43.c:3:7: warning: disable dead code elimination
//CHECK:   int Y = (++X);
//CHECK:       ^
//CHECK: de_decls_43.c:3:12: warning: side effect prevent dead code elimination
//CHECK:   int Y = (++X);
//CHECK:            ^
//CHECK: 2 warnings generated.
