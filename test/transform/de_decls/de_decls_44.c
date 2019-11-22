int foo() {
  int X = 5;
  int Y = (X = 5);
  return 5;
}
//CHECK:
