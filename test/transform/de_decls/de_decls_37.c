#define macro_37 12;
void function_37()
{
	int n;
	for(int I = 0; I < n; ++I) {
		int n_1 = n;
		for (int J = 0; J < 100; ++J) {
			int p;
		}
		for (int J = 0; J < 100; ++J) {
			int p = macro_37;
		}
	}
}
//CHECK: de_decls_37.c:6:7: warning: disable dead code elimination
//CHECK:                 int n_1 = n;
//CHECK:                     ^
//CHECK: de_decls_37.c:11:12: warning: macro prevent dead code elimination
//CHECK:                         int p = macro_37;
//CHECK:                                 ^
//CHECK: de_decls_37.c:1:20: note: expanded from macro 'macro_37'
//CHECK: #define macro_37 12;
//CHECK:                    ^
//CHECK: de_decls_37.c:11:8: warning: disable dead code elimination
//CHECK:                         int p = macro_37;
//CHECK:                             ^
//CHECK: de_decls_37.c:11:12: warning: macro prevent dead code elimination
//CHECK:                         int p = macro_37;
//CHECK:                                 ^
//CHECK: de_decls_37.c:1:20: note: expanded from macro 'macro_37'
//CHECK: #define macro_37 12;
//CHECK:                    ^
//CHECK: 4 warnings generated.
