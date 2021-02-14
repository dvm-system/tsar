int f() {
	int s = 0;
#pragma spf transform swaploops
{
	for (int i = 1; i < 10; i++) {
		s += i * i;
	}
}
	return s;
}
