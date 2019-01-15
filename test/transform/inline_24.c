struct pair
{
	int a;
	int b;
};

struct pair foo(int x, int y){
	struct pair m;
	m.a = x;
	m.b = y;
	return m;
}

int main(){
	struct pair p;

	#pragma spf transform inline
	p = foo(4, 90);
	return 0;
}
//CHECK: 
