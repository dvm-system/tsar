char bar();

char foo() {
#pragma spf assert nomacro
  {

    char C;
    return (C = bar()) && C;
  }
}
