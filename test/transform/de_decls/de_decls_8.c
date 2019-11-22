int foo_8(){
	return 597;
}
void function_8()
{
	int a = foo_8();
	int b;
	int c = 222;
	b += c;
}
//CHECK: de_decls_8.c:6:6: warning: disable dead code elimination
//CHECK:         int a = foo_8();
//CHECK:             ^
//CHECK: de_decls_8.c:6:10: warning: side effect prevent dead code elimination
//CHECK:         int a = foo_8();
//CHECK:                 ^
//CHECK: 2 warnings generated.
