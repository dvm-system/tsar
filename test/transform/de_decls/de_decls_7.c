int foo_7(){
	return 567;
}

void function_7()
{
	int a = foo_7();
	int b;
	int c = 222;
	a += c;
}
//CHECK: 
