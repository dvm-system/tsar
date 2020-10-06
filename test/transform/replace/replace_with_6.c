int bar();

int foo() {
  int (*baz)();
#pragma spf transform replace with(baz)
  return bar();
}

//CHECK: Error while processing replace_with_6.
//CHECK: replace_with_6.c:5:36: error: expected function name after 'with' clause
//CHECK: #pragma spf transform replace with(baz)
//CHECK:                                    ^
//CHECK: replace_with_6.c:4:9: note: declared here
//CHECK:   int (*baz)();
//CHECK:         ^
//CHECK: 1 error generated.
//CHECK: Error while processing replace_with_6.c.
