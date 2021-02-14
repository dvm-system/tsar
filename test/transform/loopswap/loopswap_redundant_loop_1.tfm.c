int f() {
  int s = 0;
#pragma spf transform swaploops
  {
    for (int j = 0; j < 7; ++j) {
      s -= j * 15;
    }
    for (int i = 0; i < 10; ++i) {
      s += i * 4;
    }
    for (int k = 4; k < 17; k += 3) {
      s *= k;
    }
  }
  return s;
}
