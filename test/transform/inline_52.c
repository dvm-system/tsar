int foo(){
	return 42;
}

int main(){
	int i,j,k,x = 0;


#pragma spf transform inline
	{
		for(i = 0; i < 32; i++){
			for(j = foo(); j < 32; j++){
				for(k = 0; k < 32; k++){
					
					x += 9;
				}
			}
		}
	}
	return 0;
} 
//CHECK: 
