int foo(int a){
	return a--;
}

int main(){
	int x = 50;

#pragma spf transform inline
	while(foo(x) >0){
		
		x -= 1;
	}

	return 0;
}
//CHECK: inline_64.c:9:8: warning: disable inline expansion in conditional expression of loop
//CHECK:         while(foo(x) >0){
//CHECK:               ^
//CHECK: 1 warning generated.
