int main() {
  int I;
  I = 0;
  goto head;
body:
  if (I > 10) 
    goto end;
  I = I + 1;
#pragma sapfor analysis loop(implicit)
#pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
head:
  goto body;
end:
  return 0;
}