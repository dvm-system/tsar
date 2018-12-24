#define M
#ifdef M
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