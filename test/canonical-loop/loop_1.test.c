void foo() {
  #pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis canonical
  for (int I = 0; I < 10; ++I);
}