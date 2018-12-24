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