int main() {
  int I;
  I = 0;
#pragma sapfor analysis loop(implicit)
#pragma sapfor analysis dependency(I)
head:
  if (I < 10)
    goto end;
  I = I + 1;
  goto head;
end:
  return 0;
}