int main() {
  int I;
  I = 0;
head:
  if (I < 10)
    goto end;
  I = I + 1;
  goto head;
end:
  return 0;
}