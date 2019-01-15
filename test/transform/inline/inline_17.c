int foo(){
	int x = 45;
	return x;
}

int foo_1(){
	int x = 9;
	return x;
}

int main(){
	int i = 0;

	#pragma spf transform inline
	i = foo() + foo_1();

	return 0;
}
//CHECK: 
