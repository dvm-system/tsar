int bar();
int baz();

int foo() {
#pragma spf transform replace with(baz)
  return bar();
}

//CHECK: replace_with_8.c:6:10: warning: unable to replace call expression
//CHECK:   return bar();
//CHECK:          ^
//CHECK: replace_with_8.c:5:31: note: replacement metadata not found for function 'baz'
//CHECK: #pragma spf transform replace with(baz)
//CHECK:                               ^
//CHECK: replace_with_8.c:2:5: note: declared here
//CHECK: int baz();
//CHECK:     ^
//CHECK: 1 warning generated.
