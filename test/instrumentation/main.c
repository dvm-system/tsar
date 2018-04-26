int f1(int x)
{
  int y;
  for(y = 5; y > 0; --y) {
    x += y;
  }
  return x;
}

int main()
{
  int i;
  for(i = 1; i < 3; ++i) {	  
    f1(i);
  }
  return 0;
}
