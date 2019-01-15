int foo(int a){
	return 89 + a;
}


int main(){
	int n = 7;


#pragma spf transform inline
	{
		switch(n)
		{

			case 1: 
			
			foo(0);
			break;
			case 2: foo(2);
			break;
			case 3: foo(34);
			break;
		}
	}

	return 0;
}
//CHECK: 
