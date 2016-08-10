int main() {
  int I = 0;
  #pragma sapfor analysis dependency(I)
  while (I < 10)
    I = I + 1;
  return 0;
}