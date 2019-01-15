int foo(int a){
	return a--;
}

int main(){
	int x = 50;


	while(x >0){
		#pragma spf transform inline
		x = foo(x);
	}

	return 0;
}
//CHECK: 
