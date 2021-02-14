int f() {
	int k = 0;
	int sum = 0;
#pragma spf transform swaploops
{
	for (int i = 0; i < 10; i++) {
		k = 0;
		for (int l = 1; l < 7; l++) {
			sum += i + l * k;
		}
	}
	for (int j = 0; j < 10; j++) {
		k = 7;
		for (int l = 1; l < 7; l++) {
			sum += j + l * k;
		}
	}
}
	return 0;
}
