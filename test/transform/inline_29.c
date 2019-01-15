int foo(int a){
	if (a > 0){
		#pragma spf transform inline
		return foo(a - 1);
	}
	else
		return 0;
}

int main(){

#pragma spf transform inline
	int x = foo(20);

	return 0;
}
//CHECK: inline_29.c:1:5: warning: disable inline expansion of recursive function
//CHECK: int foo(int a){
//CHECK:     ^
//CHECK: 1 warning generated.
