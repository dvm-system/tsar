int main() {
  int I;
  I = 0;
  goto head;
body:
  I = I + 1;
#pragma sapfor analysis loop(implicit)
#pragma sapfor analysis dependency(<I,4>) explicitaccess(<I,4>)
head:
  if (I < 10)
    goto end;
  goto body;
end:
  return 0;
}