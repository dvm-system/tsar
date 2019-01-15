void foo_18(int *a, int *b, int *c) {
  *a = 4;
  *b = 7;
  *c = *a + *b;
}
void function_18() {
  int a, b, c;

  for (int i = 0; i < 6; i++) {
    for (int j = 0; j < 23; j++) {
      for (;;) {
        foo_18(&a, &b, &c);
      }
    }
  }
}
