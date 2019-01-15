int foo(){
	return 91;
}


int main(){
	int x = 0;

#pragma spf transform inline
	{
		{
			x += foo();
		}
	}


	return 0;
}
//CHECK: 
