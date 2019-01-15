void foo(int* a){
	*a = 900;
}

int main(){
	int a = 0;

#pragma spf transform inline
	foo(&a);

	return 0;
}
//CHECK: 
