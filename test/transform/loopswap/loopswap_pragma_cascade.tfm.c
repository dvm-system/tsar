int f() {
  int s = 0;
#pragma spf transform swaploops
  {
    for (int j = 1; j < 7; j++) {
      s += j;
    }
    for (int i = 0; i < 10; i++) {
      s += i;
    }
#pragma spf transform swaploops
    {
      for (int b = 1; b < 7; b++) {
        s -= b;
      }
      for (int a = 0; a < 10; a++) {
        s += a;
      }
#pragma spf transform swaploops
      {
        for (int f = 1; f < 7; f++) {
          s += f;
        }
        for (int e = 0; e < 10; e++) {
          s -= e;
        }
      }
    }
  }
  int r = 10;
#pragma spf transform swaploops
  {
    for (int l = 4; l < 10; l++) {
      r += l;
    }
    for (int k = 0; k < 7; k++) {
      r += 8 + k;
    }
  }
  return s + r;
}
