int foo(int a){
	return a - 1;
}

int main(){
	int i,j = 0;

#pragma spf transform inline
	for(i = 100; i > 0; i = foo(i)){
		j++;
	}

	return 0;
}
//CHECK: inline_39.c:9:26: warning: disable inline expansion in the third section of for-loop
//CHECK:         for(i = 100; i > 0; i = foo(i)){
//CHECK:                                 ^
//CHECK: 1 warning generated.
