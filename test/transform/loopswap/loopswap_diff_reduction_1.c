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

//CHECK: loopswap_diff_reduction_1.c:4:9: warning: unable to swap loops due to different reduction kinds of variable 'acc' declared at <loopswap_diff_reduction_1.c:2:2, col:12>
//CHECK: #pragma spf transform swaploops
//CHECK:         ^
//CHECK: 1 warning generated.
