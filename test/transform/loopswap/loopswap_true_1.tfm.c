int f() {
	int sum = 0;
	int acc = 1;
#pragma spf transform swaploops
{
	for (int i = 0; i < 5; i++) {
		sum += i;
	}
	for (int j = 1; j < 10; j++) {
		acc = acc * j + sum;
	}
}
	return sum - acc;
}
