int foo_21(){
	return -111;
}
void function_21()
{
	int a = (1 + 4 + (78 * (9 / (foo_21()))));
}
//CHECK: de_decls_21.c:6:6: warning: disable dead code elimination
//CHECK:         int a = (1 + 4 + (78 * (9 / (foo_21()))));
//CHECK:             ^
//CHECK: de_decls_21.c:6:31: warning: side effect prevent dead code elimination
//CHECK:         int a = (1 + 4 + (78 * (9 / (foo_21()))));
//CHECK:                                      ^
//CHECK: 2 warnings generated.
