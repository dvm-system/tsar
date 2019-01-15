int M(){
	return 78;
}

int main()
{
	int x = 0;

	#pragma spf transform inline
	x += M();
	return 0;
}
//CHECK: 
