int sum(int(a)[5], int n) {
  int s = 0;
  for (int k = 0; k < 10; k++) {
        for (int i = 0; i < n; ++i) {
          a[i] = i;
        }
        for (int j = 0; j < n; ++j) {
          s += a[j];
        }
  }
  return s;
}