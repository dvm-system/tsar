int foo(int a1, int a2){
	return a1 * a1 + a2 + 9;
}

int main(){
	int k = 3, p = 32;
	
	#pragma spf transform inline
	int j = foo(k, p);

	return 0;
}
//CHECK: 
