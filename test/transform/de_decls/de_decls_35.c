#define macro_35 153
void function_35()
{
	if(2 > 0) {
		int i = 0;
		{
			int k;
			i = macro_35;
		}

		{
			int o;
			o = macro_35;
		}
	}	
}
//CHECK: de_decls_35.c:7:8: warning: disable dead code elimination
//CHECK:                         int k;
//CHECK:                             ^
//CHECK: de_decls_35.c:8:8: warning: macro prevent dead code elimination
//CHECK:                         i = macro_35;
//CHECK:                             ^
//CHECK: de_decls_35.c:1:18: note: expanded from macro 'macro_35'
//CHECK: #define macro_35 153
//CHECK:                  ^
//CHECK: 2 warnings generated.
