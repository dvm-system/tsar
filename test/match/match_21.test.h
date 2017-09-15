int main() {
  int I;
  #pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>) include(match_21.c:1:10)
  for (I = 0; I < 10; ++I);
  return 0;
}