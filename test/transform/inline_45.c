int foo(){
	return 95;
}


int main(){
	int x = 56;


#pragma spf transform inline
	if(x > 100)
		x += foo();
	else
		x -= foo();

	return 0;
}
//CHECK: inline_45.c:10:23: warning: unexpected directive ignored
//CHECK: #pragma spf transform inline
//CHECK:                       ^
//CHECK: inline_45.c:11:2: note: no call suitable for inline is found
//CHECK:         if(x > 100)
//CHECK:         ^
//CHECK: 1 warning generated.
