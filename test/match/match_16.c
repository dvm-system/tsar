int main() {
  int I;
  I = 0;
  goto head;
body:
  if (I > 10) 
    goto end;
  I = I + 1;
head:
  goto body;
end:
  return 0;
}