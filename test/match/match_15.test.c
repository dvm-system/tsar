int main() {
  int I;
  goto init;
#pragma sapfor analysis loop(implicit)
#pragma sapfor analysis dependency(I)
head:
  if (I > 10) 
    goto end;
  I = I + 1;
  goto head;
init:
  I = 0;
  goto head;
end:
  return 0;
}