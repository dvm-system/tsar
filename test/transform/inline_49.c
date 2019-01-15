int foo(){
	return 42;
}

int main(){
	int i,j,k,x = 0;



	for(i = 0; i < 32; i++){
		for(j = 0; j < 32; j++){
			for(k = 0; k < 32; k++){
				#pragma spf transform inline
				x += foo();
			}
		}
	}

	return 0;
} 
//CHECK: 
