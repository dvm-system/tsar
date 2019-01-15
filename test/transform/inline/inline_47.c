int foo(){
	return 95;
}


int main(){
	int x = 56;

#pragma spf transform inline
	{
		if(x > 100){
			
			x += foo();
		}
		else
			x -= foo();
	}
	return 0;
}
//CHECK: 
