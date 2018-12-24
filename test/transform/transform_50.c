int foo(int a){
	return a--;
}

int main(){
	int x = 50;

#pragma spf transform inline
	while(foo(x) >0){
		
		x -= 1;
	}

	return 0;
}