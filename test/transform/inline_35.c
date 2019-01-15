int x = 50;

int foo(){
	int x = 72;
	return x + 40;
}

int main(){
	int a = 0;

#pragma spf transform inline
	a = x + foo();

	return 0;
}
//CHECK: 
