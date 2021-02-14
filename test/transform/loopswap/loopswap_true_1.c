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
//CHECK: loopswap_true_1.c:4:9: warning: unable to swap loops due to the true dependence of variable 'sum' declared at <loopswap_true_1.c:2:2, col:12>
//CHECK: #pragma spf transform swaploops
//CHECK:         ^
//CHECK: 1 warning generated.
