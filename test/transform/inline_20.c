int foo(int a){
	return a * a;
}

int main(){
	int y, x = 5;
	
	#pragma spf transform inline
	y = foo(x);
	return 0;
}
//CHECK: 
