
int foo(){
	int a = 0;

	#ifdef M
	a += 67;
	#endif


	return a + 230;
}

int main(){
	int k;

#pragma spf transform inline
	k = foo();

	return 0;
}
//CHECK: inline_37.c:2:5: warning: disable inline expansion
//CHECK: int foo(){
//CHECK:     ^
//CHECK: inline_37.c:7:2: note: macro prevent inlining
//CHECK:         #endif
//CHECK:         ^
//CHECK: 1 warning generated.
