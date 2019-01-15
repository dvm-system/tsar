int foo(int a){
	if (a > 0)
		return foo(a - 1);
	else
		return 0;
}

int main(){

#pragma spf transform inline
	int x = foo(20);

	return 0;
}
//CHECK: 
