
int foo_40(){
	return 56;
}
#define macro_40 foo_40
void function_40()
{
	for(int i = 0; i < 999; macro_40()){}
}
//CHECK: 
