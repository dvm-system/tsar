int foo_10(){
	return -45;
}
void function_10()
{
	int a = 0, b, c = foo_10();
	b = a;
	c++;
}
//CHECK: 
