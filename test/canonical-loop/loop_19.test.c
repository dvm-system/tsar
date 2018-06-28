void foo() {
  int K = 1;
  #pragma sapfor analysis dependency(<I,4>) shared(<K,4>) explicitaccess(<I,4>, <K,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis canonical
  for (int I = 0; I < 10; I = I + K + 1);
}
