int main() {
  int I;
  #pragma sapfor analysis dependency(I)
  for (I = 0; I < 10; ++I);
  return 0;
}