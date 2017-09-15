int main() {
  int I = 0;
  #pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
  do
    I = I + 1;
  while (I < 10);
  return 0;
}