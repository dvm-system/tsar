int f() {
	int s = 0;
#pragma spf transform swaploops
{
	s = 15;
	for (int i = 0; i < 10; i++) {
		s += i;
	}
	for (int k = 0; k < 15; k++) {
		s += k;
	}
}
	return s;
}
