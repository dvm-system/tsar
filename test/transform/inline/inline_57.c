int X = 9;

void foo(int X){
	X++;
}

int main(){

#pragma spf transform inline
	foo(71);

	return 0;
}
//CHECK: 
