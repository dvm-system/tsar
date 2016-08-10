int main() {
  int I;
  #pragma sapfor analysis dependency(I) include(match_21.c:1:10)
  for (I = 0; I < 10; ++I);
  return 0;
}