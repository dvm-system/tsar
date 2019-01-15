void function_13()
{
	int n;
	for(int I = 0,j = 0; I < n; ++I) {
		int n_1 = n;
	}
}
//CHECK: de_decls_13.c:4:16: warning: disable dead code elimination
//CHECK:         for(int I = 0,j = 0; I < n; ++I) {
//CHECK:                       ^
//CHECK: de_decls_13.c:4:10: warning: live declaration prevent dead code elimination
//CHECK:         for(int I = 0,j = 0; I < n; ++I) {
//CHECK:                 ^
//CHECK: 2 warnings generated.
