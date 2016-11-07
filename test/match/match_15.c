int main() {
  int I;
  goto init;
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