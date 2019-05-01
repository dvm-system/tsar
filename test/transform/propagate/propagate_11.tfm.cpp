void foo(double &X) {
#pragma spf assert nomacro
  {

    float Y = 4.21;
    X = 4.21000004;
  }
}
