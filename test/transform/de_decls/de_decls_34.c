#define macro_34 103
void function_34()
{
	if(2 > 0) {
		int i = 0;
		{
			int k;
			i = macro_34;
		}

		{int o;}
	}	
}
//CHECK: de_decls_34.c:5:7: warning: disable dead code elimination
//CHECK:                 int i = 0;
//CHECK:                     ^
//CHECK: de_decls_34.c:8:8: warning: macro prevent dead code elimination
//CHECK:                         i = macro_34;
//CHECK:                             ^
//CHECK: de_decls_34.c:1:18: note: expanded from macro 'macro_34'
//CHECK: #define macro_34 103
//CHECK:                  ^
//CHECK: de_decls_34.c:7:8: warning: disable dead code elimination
//CHECK:                         int k;
//CHECK:                             ^
//CHECK: de_decls_34.c:8:8: warning: macro prevent dead code elimination
//CHECK:                         i = macro_34;
//CHECK:                             ^
//CHECK: de_decls_34.c:1:18: note: expanded from macro 'macro_34'
//CHECK: #define macro_34 103
//CHECK:                  ^
//CHECK: 4 warnings generated.
