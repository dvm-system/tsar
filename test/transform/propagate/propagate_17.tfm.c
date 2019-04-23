char foo(char X, char Y) {

  char C = X;
  return X && (C = X > 0 ? X : X + 1) && (X > 0 ? X : X + 1);
}
