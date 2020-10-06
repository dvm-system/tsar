int bar();
int baz();

int foo() {
#pragma spf transform replace with(baz) with(foo)
  return bar();
}

//CHECK: Error while processing replace_with_5.
//CHECK: replace_with_5.c:5:41: error: directive '#pragma spf transform replace' cannot contain more than one 'with' clause
//CHECK: #pragma spf transform replace with(baz) with(foo)
//CHECK:                                         ^
//CHECK: 1 error generated.
//CHECK: Error while processing replace_with_5.c.
