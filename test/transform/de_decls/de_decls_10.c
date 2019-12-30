int foo_10(){
	return -45;
}
int function_10()
{
	int a = 0, b, c = foo_10();
	b = a;
	c++;
        return b + c;
}
//CHECK: 
