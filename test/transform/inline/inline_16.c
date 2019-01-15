int M(){
	return 78;
}

int foo(int a){

	return a + 567;
}

int main()
{
	int x = 0;

	#pragma spf transform inline
	x += foo(
		M());
	return 0;
}
//CHECK: 
