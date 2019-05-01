char foo(char X, char Y) {
#pragma spf assert nomacro
  {

    char C = Y;
    return (C = X);
  }
}
