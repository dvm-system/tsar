int main() {
  int I, J;
  #pragma sapfor analysis dependency(I) private(J)
  for (I = 0; I < 10; ++I) {
    J  = 0;
    #pragma sapfor analysis dependency(J)
    for (; J < 10; ++J);
  }
  return 0;
}