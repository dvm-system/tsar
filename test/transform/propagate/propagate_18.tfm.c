char bar();

char foo() {

  char C;
  return (C = bar()) && C;
}
