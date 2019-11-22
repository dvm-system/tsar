int foo_9(){
	return -345;
}
void function_9()
{
	int a = 0, b, c = foo_9();
}
//CHECK: de_decls_9.c:6:16: warning: disable dead code elimination
//CHECK:         int a = 0, b, c = foo_9();
//CHECK:                       ^
//CHECK: de_decls_9.c:6:20: warning: side effect prevent dead code elimination
//CHECK:         int a = 0, b, c = foo_9();
//CHECK:                           ^
//CHECK: de_decls_9.c:6:6: warning: disable dead code elimination
//CHECK:         int a = 0, b, c = foo_9();
//CHECK:             ^
//CHECK: de_decls_9.c:6:16: warning: live declaration prevent dead code elimination
//CHECK:         int a = 0, b, c = foo_9();
//CHECK:                       ^
//CHECK: de_decls_9.c:6:13: warning: disable dead code elimination
//CHECK:         int a = 0, b, c = foo_9();
//CHECK:                    ^
//CHECK: de_decls_9.c:6:16: warning: live declaration prevent dead code elimination
//CHECK:         int a = 0, b, c = foo_9();
//CHECK:                       ^
//CHECK: 6 warnings generated.
