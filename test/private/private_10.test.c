int main() {
  int I, X;
  I = 0;
  #pragma sapfor analysis lastprivate(X) dependency(I)
  do {
    X = I;
    ++I;
  } while (I < 10);
  X = X + 1;
  return 0;
}