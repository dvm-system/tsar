int function_5()
{
	int a, b, c;
	c = 0;
	b = 98;
        return c + b;
}
//CHECK: de_decls_5.c:3:6: warning: disable dead code elimination
//CHECK:         int a, b, c;
//CHECK:             ^
//CHECK: de_decls_5.c:3:12: warning: live declaration prevent dead code elimination
//CHECK:         int a, b, c;
//CHECK:                   ^
//CHECK: 2 warnings generated.
