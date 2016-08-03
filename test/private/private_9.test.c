int main() {
  int I;
  #pragma sapfor analysis dependency(I) private(X)
  for (I = 0; I < 10; ++I) {
    int X = I;   
  }
}
