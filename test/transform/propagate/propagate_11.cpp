void foo(double &X) {
#pragma spf transform propagate
  float Y = 4.21;
  X = Y;
}
//CHECK: 
