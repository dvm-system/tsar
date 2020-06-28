int bar();

int foo() {
  int (*baz)() = bar;
#pragma spf transform replace with(foo)
  return baz();
}

//CHECK: replace_with_9.c:6:10: warning: unable to replace indirect call expression
//CHECK:   return baz();
//CHECK:          ^
//CHECK: 1 warning generated.
