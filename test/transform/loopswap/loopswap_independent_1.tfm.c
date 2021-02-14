int f() {
  int a = 5, b = 1, c = 0, d = 4;
#pragma spf transform swaploops
  {
    for (int j = 1; j < 7; j++) {
      c += j;
      d *= c * 3;
    }
    for (int i = 0; i < 10; i++) {
      a += i;
      b *= i * 2;
    }
  }
  return a + b + c + d;
}
