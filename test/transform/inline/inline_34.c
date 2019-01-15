int foo(){
	return 25;
}

int main(){
	int x;

#pragma spf transform inline
	x = (2 < 3)?foo():0;


	return 0;
}
//CHECK: inline_34.c:9:14: warning: disable inline expansion in conditional operator
//CHECK:         x = (2 < 3)?foo():0;
//CHECK:                     ^
//CHECK: 1 warning generated.
