int X = 9;

void foo(){
	X++;
}

int main(){

#pragma spf transform inline
	foo();

	return 0;
}
//CHECK: 
