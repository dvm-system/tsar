int foo() { return 34; }
int main(){
	int x;
#ifdef INL
# pragma spf transform inline
#endif
	x = foo();
	return 0;
}
//CHECK: 
