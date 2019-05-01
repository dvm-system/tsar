float foo(float X) {
#pragma spf assert nomacro
  {

    float Y, Z;
    Y = X;
    Z = X;
    return X + X;
  }
}
