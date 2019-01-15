int foo(){
	return 100;
}

int main(){
	int i,j = 0;

#pragma spf transform inline
	for(i = foo(); i > 0; i--){
		j++;
	}

	return 0;
}
//CHECK: 
