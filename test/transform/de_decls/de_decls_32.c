#define macro_32 451
void function_32()
{
	if(2 > 0) {
		int i = 0;
		{
			int k = macro_32;
		}
	}
}
//CHECK: de_decls_32.c:5:7: warning: disable dead code elimination
//CHECK:                 int i = 0;
//CHECK:                     ^
//CHECK: de_decls_32.c:7:12: warning: macro prevent dead code elimination
//CHECK:                         int k = macro_32;
//CHECK:                                 ^
//CHECK: de_decls_32.c:1:18: note: expanded from macro 'macro_32'
//CHECK: #define macro_32 451
//CHECK:                  ^
//CHECK: de_decls_32.c:7:8: warning: disable dead code elimination
//CHECK:                         int k = macro_32;
//CHECK:                             ^
//CHECK: de_decls_32.c:7:12: warning: macro prevent dead code elimination
//CHECK:                         int k = macro_32;
//CHECK:                                 ^
//CHECK: de_decls_32.c:1:18: note: expanded from macro 'macro_32'
//CHECK: #define macro_32 451
//CHECK:                  ^
//CHECK: 4 warnings generated.
