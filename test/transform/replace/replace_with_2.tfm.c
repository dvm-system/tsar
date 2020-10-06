void bar() {}

void foo() {
#pragma spf metadata replace(bar())
}

void baz() { foo(); }
