int foo(){
	return 42;
}

int main(){
	int i,j,k,x = 0, y;


#pragma spf transform inline
	{
		for(i = 0; i < 32; i++){
			for(j = 0; j < 32; j++){
				for(k = 0; k < 32; k++){
					
					x += foo();
				}
			}
			y = (i != 0)?foo():0;
		}
	}
	return 0;
} 