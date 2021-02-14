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
//CHECK: loopswap_private_fake.c:4:9: warning: unable to swap loops due to the output dependence of variable 'sum' declared at <loopswap_private_fake.c:3:2, col:12>
//CHECK: #pragma spf transform swaploops
//CHECK:         ^
//CHECK: 1 warning generated.
