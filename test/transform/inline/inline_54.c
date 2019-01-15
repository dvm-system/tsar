int N = 50;

void foo(int* a){
	int i = 0;
	for(i = 0; i < N; i++)
		a[i] = 0;
}

int main(){
	int b[N];


#pragma spf transform inline
	foo(b);

	return 0;
}
//CHECK: 
