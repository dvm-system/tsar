int main() {
  int I;
  I = 0;
  if (I > 5)
    goto head;
  goto head;
head:
  if (I < 10)
    goto end;
  I = I + 1;
  goto head;
end:
  return 0;
}