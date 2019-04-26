float foo() {
#pragma spf transform propagate
{
  float X = 2.1;
  double Y = 2.1;
  int Z = (int)2.1;
  return X + Y + Z;
}
}
//CHECK: 
