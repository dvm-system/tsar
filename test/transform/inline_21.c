int foo(int a){
	return a * a;
}

int main(){
	int x = 5;
	
	#pragma spf transform inline
	x = foo(x);
	return 0;
}
//CHECK: 
