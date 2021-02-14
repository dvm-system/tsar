int f() {
  int k = 0;
  int sum = 0;
#pragma spf transform swaploops
  {
    for (int j = 0; j < 10; j++) {
      k = 8;
      sum += j * j + k;
    }
    for (int i = 0; i < 10; i++) {
      k = 7;
      sum += i * i + k;
    }
  }
  return sum;
}
