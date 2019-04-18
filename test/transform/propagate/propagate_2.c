int foo(int X) {
  int Y;
#pragma spf transform propagate
{ 
  if (X < 0)
    Y = X;
  else
    Y = X;
  return Y + X;
}
}
//CHECK: 
