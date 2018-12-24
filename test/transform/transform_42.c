int X = 9;
void foo(){
	X++;
}
void foo_2(){
	int X = 0;

	X++;
#pragma spf transform inline
	foo();
}
int main(){

#pragma spf transform inline
	foo_2();

	return 0;
}