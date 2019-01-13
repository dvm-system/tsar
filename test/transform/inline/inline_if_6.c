int f() {
  return 1;
}

int g(int X) {
  #pragma spf transform inline
  if (f() && X)
    return X;
  return 0;
}
//CHECK: 
