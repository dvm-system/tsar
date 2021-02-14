int f() {
	int b = 18;
	int sum = 0;
#pragma spf transform swaploops
{
	for (int i = 0; i < 7; i++) {
		sum += b * i;
	}
	for (int j = 0; j < 5; j++) {
		b += j * 10;
	}
}
	return sum - b;
}
