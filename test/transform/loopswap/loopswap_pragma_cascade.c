int f() {
	int s = 0;
#pragma spf transform swaploops
{
	for (int i = 0; i < 10; i++) {
		s += i;
	}
	for (int j = 1; j < 7; j++) {
		s += j;
	}
	#pragma spf transform swaploops
	{	
		for (int a = 0; a < 10; a++) {
			s += a;
		}
		for (int b = 1; b < 7; b++) {
			s -= b;
		}
		#pragma spf transform swaploops
		{	
			for (int e = 0; e < 10; e++) {
				s -= e;
			}
			for (int f = 1; f < 7; f++) {
				s += f;
			}
		}
	}
}
	int r = 10;
#pragma spf transform swaploops
{
	for (int k = 0; k < 7; k++) {
		r += 8 + k;
	}
	for (int l = 4; l < 10; l++) {
		r += l;
	}
}
	return s + r;
}
//CHECK: 
