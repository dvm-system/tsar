int main() {
  int I, X;
  I = 0;
  #pragma sapfor analysis firstprivate(X) secondtolastprivate(X) dependency(I)
  while (I < 10) {
    X = I;
    ++I;
  };
  X = X + 1;
  return 0;
}