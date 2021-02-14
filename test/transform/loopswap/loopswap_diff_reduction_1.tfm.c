int f() {
	int acc = 0;
	int sum = 100;
#pragma spf transform swaploops
{
	for (int i = 0; i < 7; ++i) {
		acc += i + 10;
		sum += i * 15;
	}
	for (int j = 1; j < 10; ++j) {
		acc *= j;
		sum -= j;
	}
}
	return acc + sum;
}
