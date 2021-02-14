int f() {
  int sum = 0, res = 1;
#pragma spf transform swaploops
  {
    for (int j = 2; j < 10; j++)
      res *= j;
    for (int i = 0; i < 5; i++) {
      sum += 2;
      sum *= 1;
    }
  }
  return sum * res;
}
