int foo(int a){
	return a - 1;
}

int main(){
	int i,j = 0;

#pragma spf transform inline
	for(i = 100; foo(i) > 0; i--){
		j++;
	}

	return 0;
}
//CHECK: inline_38.c:9:15: warning: disable inline expansion in conditional expression of loop
//CHECK:         for(i = 100; foo(i) > 0; i--){
//CHECK:                      ^
//CHECK: 1 warning generated.
