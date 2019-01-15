void function_31() {
  int v = 0;
  // 'a' is not dead because it is read when condition is evaluated.
  if (int a = 567)
    v++;
} 
//CHECK: 
