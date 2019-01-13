int f(int I) {
  if (I)
    return 5;
  return 10;
}

void g() {
#pragma spf transform inline
  f(1);
}
//CHECK: 
