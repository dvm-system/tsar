
int foo(){
	int a = 0;

	#ifdef M
	a += 67;
	#endif


	return a + 230;
}

int main(){
	int k;

#pragma spf transform inline
	k = foo();

	return 0;
}