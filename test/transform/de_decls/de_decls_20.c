int foo_20(int* a, int*b, int* c){
	*a = 4;
	*b = 7;
	*c = *a + *b; 
	return 0;
}
void function_20()
{
	int a, b, c;

	for(int i = 0; i < 6; i++){
		for(int j = 0; j < 23; j++){
			for(int k = -56;;){
				int d = (1+(1+(1+(1+(1+(1+(1+(1+(1+(1+(foo_20(&a, &b, &k))))))))))));
				c++;
			}
		}
	}
}
//CHECK: de_decls_20.c:14:9: warning: disable dead code elimination
//CHECK:                                 int d = (1+(1+(1+(1+(1+(1+(1+(1+(1+(1+(foo_20(&a, &b, &k))))))))))));
//CHECK:                                     ^
//CHECK: de_decls_20.c:14:44: warning: side effect prevent dead code elimination
//CHECK:                                 int d = (1+(1+(1+(1+(1+(1+(1+(1+(1+(1+(foo_20(&a, &b, &k))))))))))));
//CHECK:                                                                        ^
//CHECK: 2 warnings generated.
