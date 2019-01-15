void function_39()
{
	{
		int a;
		{
			int b = a;
			{

						{
							int c;

					#ifdef macro_39
						}
						{
					#endif
							int d;
						}

			}
		}
	}
}
//CHECK: de_decls_39.c:6:8: warning: disable dead code elimination
//CHECK:                         int b = a;
//CHECK:                             ^
//CHECK: de_decls_39.c:15:6: warning: macro prevent dead code elimination
//CHECK:                                         #endif
//CHECK:                                         ^
//CHECK: de_decls_39.c:10:12: warning: disable dead code elimination
//CHECK:                                                         int c;
//CHECK:                                                             ^
//CHECK: de_decls_39.c:15:6: warning: macro prevent dead code elimination
//CHECK:                                         #endif
//CHECK:                                         ^
//CHECK: de_decls_39.c:16:12: warning: disable dead code elimination
//CHECK:                                                         int d;
//CHECK:                                                             ^
//CHECK: de_decls_39.c:15:6: warning: macro prevent dead code elimination
//CHECK:                                         #endif
//CHECK:                                         ^
//CHECK: 6 warnings generated.
