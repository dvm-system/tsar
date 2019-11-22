int function_4()
{
	int a, b, c;
	a = 0;
	c = 98;	
        return a + c;
}
//CHECK: de_decls_4.c:3:9: warning: disable dead code elimination
//CHECK:         int a, b, c;
//CHECK:                ^
//CHECK: de_decls_4.c:3:12: warning: live declaration prevent dead code elimination
//CHECK:         int a, b, c;
//CHECK:                   ^
//CHECK: 2 warnings generated.
