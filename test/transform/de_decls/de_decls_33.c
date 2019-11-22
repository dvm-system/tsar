#define macro_33 4351
void function_33()
{
	if(2 > 0) {
		int i = 0;
		{
			int k;
			i = macro_33;
		}
	}
}
//CHECK: de_decls_33.c:5:7: warning: disable dead code elimination
//CHECK:                 int i = 0;
//CHECK:                     ^
//CHECK: de_decls_33.c:8:8: warning: macro prevent dead code elimination
//CHECK:                         i = macro_33;
//CHECK:                             ^
//CHECK: de_decls_33.c:1:18: note: expanded from macro 'macro_33'
//CHECK: #define macro_33 4351
//CHECK:                  ^
//CHECK: de_decls_33.c:7:8: warning: disable dead code elimination
//CHECK:                         int k;
//CHECK:                             ^
//CHECK: de_decls_33.c:8:8: warning: macro prevent dead code elimination
//CHECK:                         i = macro_33;
//CHECK:                             ^
//CHECK: de_decls_33.c:1:18: note: expanded from macro 'macro_33'
//CHECK: #define macro_33 4351
//CHECK:                  ^
//CHECK: 4 warnings generated.
