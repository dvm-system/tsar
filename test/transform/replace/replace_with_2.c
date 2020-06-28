void bar() { }

void foo() {
#pragma spf metadata replace(bar())
}

void baz() {
#pragma spf transform replace with(foo)
  bar();
}
//CHECK: 
