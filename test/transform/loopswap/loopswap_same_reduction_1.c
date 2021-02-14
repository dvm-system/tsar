int f() {
  int sum = 0;
#pragma spf transform swaploops
  {
    for (int j = 0; j < 7; j++) {
      sum -= j * 4;
    }
    for (int i = 0; i < 5; ++i) {
      sum += i;
    }
  }
  return sum;
}
//CHECK: 
