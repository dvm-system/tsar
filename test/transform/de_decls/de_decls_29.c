void function_29()
{
	#ifdef macro_28
		int a, b, c;
	#else
		int i, j, k;
		i = 67;
	#endif	
}
//CHECK: de_decls_29.c:6:7: warning: disable dead code elimination
//CHECK:                 int i, j, k;
//CHECK:                     ^
//CHECK: de_decls_29.c:8:2: warning: macro prevent dead code elimination
//CHECK:         #endif  
//CHECK:         ^
//CHECK: de_decls_29.c:6:10: warning: disable dead code elimination
//CHECK:                 int i, j, k;
//CHECK:                        ^
//CHECK: de_decls_29.c:8:2: warning: macro prevent dead code elimination
//CHECK:         #endif  
//CHECK:         ^
//CHECK: de_decls_29.c:6:13: warning: disable dead code elimination
//CHECK:                 int i, j, k;
//CHECK:                           ^
//CHECK: de_decls_29.c:8:2: warning: macro prevent dead code elimination
//CHECK:         #endif  
//CHECK:         ^
//CHECK: 6 warnings generated.
