#define macro_26 45
void function_26()
{
	int a, b, c = macro_26;
}
//CHECK: de_decls_26.c:4:6: warning: disable dead code elimination
//CHECK:         int a, b, c = macro_26;
//CHECK:             ^
//CHECK: de_decls_26.c:4:16: warning: macro prevent dead code elimination
//CHECK:         int a, b, c = macro_26;
//CHECK:                       ^
//CHECK: de_decls_26.c:1:18: note: expanded from macro 'macro_26'
//CHECK: #define macro_26 45
//CHECK:                  ^
//CHECK: de_decls_26.c:4:9: warning: disable dead code elimination
//CHECK:         int a, b, c = macro_26;
//CHECK:                ^
//CHECK: de_decls_26.c:4:16: warning: macro prevent dead code elimination
//CHECK:         int a, b, c = macro_26;
//CHECK:                       ^
//CHECK: de_decls_26.c:1:18: note: expanded from macro 'macro_26'
//CHECK: #define macro_26 45
//CHECK:                  ^
//CHECK: de_decls_26.c:4:12: warning: disable dead code elimination
//CHECK:         int a, b, c = macro_26;
//CHECK:                   ^
//CHECK: de_decls_26.c:4:16: warning: macro prevent dead code elimination
//CHECK:         int a, b, c = macro_26;
//CHECK:                       ^
//CHECK: de_decls_26.c:1:18: note: expanded from macro 'macro_26'
//CHECK: #define macro_26 45
//CHECK:                  ^
//CHECK: 6 warnings generated.
