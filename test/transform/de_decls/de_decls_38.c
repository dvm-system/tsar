void function_38()
{
	{
		int a = 9;
		{
			int b;
			{
				#ifdef macro_38
					int c;
				#endif
			}
		}
	}
}
//CHECK: de_decls_38.c:4:7: warning: disable dead code elimination
//CHECK:                 int a = 9;
//CHECK:                     ^
//CHECK: de_decls_38.c:10:5: warning: macro prevent dead code elimination
//CHECK:                                 #endif
//CHECK:                                 ^
//CHECK: de_decls_38.c:6:8: warning: disable dead code elimination
//CHECK:                         int b;
//CHECK:                             ^
//CHECK: de_decls_38.c:10:5: warning: macro prevent dead code elimination
//CHECK:                                 #endif
//CHECK:                                 ^
//CHECK: 4 warnings generated.
