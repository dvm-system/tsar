int foo(int a){
	return a - 1;
}

int main(){
	int i,j = 0;

#pragma spf transform inline
	for(i = 100; i > 0; i = foo(i)){
		j++;
	}

	return 0;
}