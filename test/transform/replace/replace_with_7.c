int bar();

int foo() {
#pragma spf transform replace with(foo)
  return bar();
}

//CHECK: replace_with_7.c:5:10: warning: unable to replace call expression
//CHECK:   return bar();
//CHECK:          ^
//CHECK: replace_with_7.c:4:31: note: replacement metadata not found for function 'foo'
//CHECK: #pragma spf transform replace with(foo)
//CHECK:                               ^
//CHECK: replace_with_7.c:3:5: note: declared here
//CHECK: int foo() {
//CHECK:     ^
//CHECK: 1 warning generated.
