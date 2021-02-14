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
//CHECK: loopswap_anti_1.c:4:9: warning: unable to swap loops due to the anti dependence of variable 'b' declared at <loopswap_anti_1.c:2:2, col:10>
//CHECK: #pragma spf transform swaploops
//CHECK:         ^
//CHECK: 1 warning generated.
