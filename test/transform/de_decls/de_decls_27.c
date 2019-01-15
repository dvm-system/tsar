#define macro_27 90
void function_27()
{
	int a, b;
	int c = macro_27;
}
//CHECK: de_decls_27.c:4:6: warning: disable dead code elimination
//CHECK:         int a, b;
//CHECK:             ^
//CHECK: de_decls_27.c:5:10: warning: macro prevent dead code elimination
//CHECK:         int c = macro_27;
//CHECK:                 ^
//CHECK: de_decls_27.c:1:18: note: expanded from macro 'macro_27'
//CHECK: #define macro_27 90
//CHECK:                  ^
//CHECK: de_decls_27.c:4:9: warning: disable dead code elimination
//CHECK:         int a, b;
//CHECK:                ^
//CHECK: de_decls_27.c:5:10: warning: macro prevent dead code elimination
//CHECK:         int c = macro_27;
//CHECK:                 ^
//CHECK: de_decls_27.c:1:18: note: expanded from macro 'macro_27'
//CHECK: #define macro_27 90
//CHECK:                  ^
//CHECK: de_decls_27.c:5:6: warning: disable dead code elimination
//CHECK:         int c = macro_27;
//CHECK:             ^
//CHECK: de_decls_27.c:5:10: warning: macro prevent dead code elimination
//CHECK:         int c = macro_27;
//CHECK:                 ^
//CHECK: de_decls_27.c:1:18: note: expanded from macro 'macro_27'
//CHECK: #define macro_27 90
//CHECK:                  ^
//CHECK: 6 warnings generated.
