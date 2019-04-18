float foo(float X) {
#pragma spf transform propagate
  float Y, Z;
  Y = X;
  Z = X;
  return Y + Z;
}
//CHECK: 
