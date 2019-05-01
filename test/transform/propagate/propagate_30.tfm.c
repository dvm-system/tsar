float foo() {

#pragma spf assert nomacro
  {
    float X = 2.1;
    double Y = 2.1;
    int Z = (int)2.1;
    return 2.0999999 + 2.1000000000000001 + 2;
  }
}
