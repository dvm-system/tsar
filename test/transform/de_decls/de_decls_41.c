int foo(int A) {
  return 5;
}
//CHECK: de_decls_41.c:1:13: warning: disable dead code elimination for function parameters
//CHECK: int foo(int A) {
//CHECK:             ^
//CHECK: 1 warning generated.
