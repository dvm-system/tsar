int foo(){
	return 2345;
}


int main(){
	int i = 0;

	if(2 > 3) {
		#pragma spf transform inline
		i = foo();
	}
	return 0;
}
//CHECK: inline_30.c:11:7: warning: disable inline expansion of unreachable call
//CHECK:                 i = foo();
//CHECK:                     ^
//CHECK: 1 warning generated.
