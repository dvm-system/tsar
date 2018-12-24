int foo(){
	return 25;
}

int main(){
	int x;

#pragma spf transform inline
	x = (2 < 3)?foo():0;


	return 0;
}