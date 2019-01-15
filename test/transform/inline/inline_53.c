int foo_1(){
	return 37;
}

int foo_2(){
	
#pragma spf transform inline
	return foo_1() + 21;
}

int foo_3(){

#pragma spf transform inline
	return foo_1() + foo_2();
}

int main(){
	int x = 0;


#pragma spf transform inline
	x = foo_3();

	return 0;
}
//CHECK: 
