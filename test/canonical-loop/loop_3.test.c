void foo() {
  int I;
  #pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis non-canonical
  for (I = 0; I < 10; --I);
}