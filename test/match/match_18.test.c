int main() {
  int I;
  #pragma sapfor analysis unavailable
  for (I = 0; I < 10; ++I)
    return 0;
}