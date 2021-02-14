int f() {
	int c = 0;
#pragma spf transform swaploops
int k = 5;
{
	for (int i = 0; i < 10; i++)
		c += i;
}
	return c;
}
