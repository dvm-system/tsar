int foo(int a){
	return 89 + a;
}


int main(){
	int n = 7;


#pragma spf transform inline
	switch(n)
	{

		case 1: 
		
		foo(0);
		break;
		case 2: foo(2);
		break;
		case 3: foo(34);
		break;
	}

	return 0;
}
//CHECK: inline_61.c:10:23: warning: unexpected directive ignored
//CHECK: #pragma spf transform inline
//CHECK:                       ^
//CHECK: inline_61.c:11:2: note: no call suitable for inline is found
//CHECK:         switch(n)
//CHECK:         ^
//CHECK: 1 warning generated.
