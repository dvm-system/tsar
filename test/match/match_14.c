int main() {
  int I;
  I = 0;
  goto head;
body:
  I = I + 1;
head:
  if (I < 10)
    goto end;
  goto body;
end:
  return 0;
}