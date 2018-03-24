void foo() {
  #pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
  #pragma sapfor analysis perfect
  #pragma sapfor analysis canonical
  for (long I = 0; 10 > I; I = I + (unsigned)1);
}