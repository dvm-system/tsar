int function_3()
{
	int a, b, c;
	a = 0;
	b = 98;
        return a + b;
}
//CHECK: de_decls_3.c:3:12: warning: disable dead code elimination
//CHECK:         int a, b, c;
//CHECK:                   ^
//CHECK: de_decls_3.c:3:9: warning: live declaration prevent dead code elimination
//CHECK:         int a, b, c;
//CHECK:                ^
//CHECK: 2 warnings generated.
