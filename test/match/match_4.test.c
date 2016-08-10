int main() {
  int I = 0;
  if (I < 5)
    goto label;
label:
  #pragma sapfor analysis dependency(I)
  do 
    I = I + 1; 
  while (I < 10);
  return 0;
}