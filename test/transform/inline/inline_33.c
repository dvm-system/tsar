int foo(){
	return 25;
}

int main(){
	int x;

#pragma spf transform inline
	x = (2 > 3)?0:foo();


	return 0;
}
//CHECK: inline_33.c:9:16: warning: disable inline expansion in conditional operator
//CHECK:         x = (2 > 3)?0:foo();
//CHECK:                       ^
//CHECK: 1 warning generated.
