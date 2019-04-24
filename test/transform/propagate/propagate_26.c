int foo(int Z) {
#pragma spf transform propagate
  int X = Z;
  for (int Z = 5; Z < X; ++Z) {
    if (Z == X - 1)
      return X;
  }
  return X + 1;
}
//CHECK: 
