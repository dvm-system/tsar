int foo_14(){
	return 789;
}
void function_14()
{
	int n;
	for(int I = 0, p = foo_14(); I < n; ++I) {
		int n_1 = n;
		for (int j = foo_14(), J = 0; J < n; ++J) {
		}
	}
}
//CHECK: de_decls_14.c:7:17: warning: disable dead code elimination
//CHECK:         for(int I = 0, p = foo_14(); I < n; ++I) {
//CHECK:                        ^
//CHECK: de_decls_14.c:7:21: warning: side effect prevent dead code elimination
//CHECK:         for(int I = 0, p = foo_14(); I < n; ++I) {
//CHECK:                            ^
//CHECK: de_decls_14.c:9:12: warning: disable dead code elimination
//CHECK:                 for (int j = foo_14(), J = 0; J < n; ++J) {
//CHECK:                          ^
//CHECK: de_decls_14.c:9:16: warning: side effect prevent dead code elimination
//CHECK:                 for (int j = foo_14(), J = 0; J < n; ++J) {
//CHECK:                              ^
//CHECK: 4 warnings generated.
