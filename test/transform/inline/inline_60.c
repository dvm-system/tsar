int foo(int A) {
  return 89 + A;
}


int main(int Argc, char **Argv){
  switch(Argc) {
    case 1: 
#pragma spf transform inline
      foo(0);
      break;
  case 2: foo(2); break;
  case 3: foo(34); break;
  }
  return 0;
}
//CHECK: 
