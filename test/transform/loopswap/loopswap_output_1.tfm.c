int f() {
  int k, l;
#pragma spf transform swaploops
  {
    for (int j = 0; j < 15; ++j)
      l = j;
    for (int i = 0; i < 10; ++i)
      k = i;
  }
  return k + l;
}
