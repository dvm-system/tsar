
float foo(float X) {
#pragma spf transform propagate
  float Y;
  float Z;
  Y = X;
  Z = X;
  return Y + Z;
}
//CHECK: 
