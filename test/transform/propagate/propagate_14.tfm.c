
float foo(float X) {
#pragma spf assert nomacro
  {

    float Y;
    float Z;
    Y = X;
    Z = X;
    return X + Z;
  }
}
