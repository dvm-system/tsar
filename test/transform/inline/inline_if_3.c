int f1() {
  return 0;
}

void f(int I) {
#pragma spf transform inline
{
  if (I == 1) {}
  else if (f1()) {}
  else {}
}
}
//CHECK: 
