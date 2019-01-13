int f2() { return 2; }
int f3() {
  #pragma spf transform inline
  return f2() + 3;
}

int f1() { 
#pragma spf transform inline
  return f3() + 1;
}
//CHECK: 
