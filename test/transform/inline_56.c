int X = 9;
void foo(){
	X++;
}
void foo_2(){
	int X = 0;

	X++;
#pragma spf transform inline
	foo();
}
int main(){

#pragma spf transform inline
	foo_2();

	return 0;
}
//CHECK: inline_56.c:10:2: warning: disable inline expansion
//CHECK:         foo();
//CHECK:         ^
//CHECK: inline_56.c:1:5: note: hidden external dependence prevents inlining
//CHECK: int X = 9;
//CHECK:     ^
//CHECK: inline_56.c:6:6: note: declaration hides other declaration
//CHECK:         int X = 0;
//CHECK:             ^
//CHECK: inline_56.c:10:2: warning: disable inline expansion
//CHECK:         foo();
//CHECK:         ^
//CHECK: inline_56.c:1:5: note: hidden external dependence prevents inlining
//CHECK: int X = 9;
//CHECK:     ^
//CHECK: inline_56.c:6:6: note: declaration hides other declaration
//CHECK:         int X = 0;
//CHECK:             ^
//CHECK: 2 warnings generated.
