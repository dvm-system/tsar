#define M
#ifdef M
    // We do not check that function definition is placed under condition.
	// So, inlining is valid in this case.
	int foo(){
		return 34;
	}
#endif

int main(){
	int x;

#pragma spf transform inline
	x = foo();

	return 0;
}
//CHECK: 
