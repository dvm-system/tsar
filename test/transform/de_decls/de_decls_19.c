void foo_19(int* a, int*b, int* c){
	*a = 4;
	*b = 7;
	*c = *a + *b; 
}
void function_19()
{
	int a, b, c;

	for(int i = 0; i < 6; i++){
		for(int j = 0; j < 23; j++){
			for(int k = -56;;){
				foo_19(&a, &b, &k);
			}
		}
	}
}
//CHECK: de_decls_19.c:8:12: warning: disable dead code elimination
//CHECK:         int a, b, c;
//CHECK:                   ^
//CHECK: de_decls_19.c:8:9: warning: live declaration prevent dead code elimination
//CHECK:         int a, b, c;
//CHECK:                ^
//CHECK: 2 warnings generated.
