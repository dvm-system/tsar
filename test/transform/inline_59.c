int foo(int a){

	return a + 56;
}


int main(){

#pragma spf transform inline
	{
		foo(foo(foo(3)));
	}
	return 0;
}
//CHECK: 
