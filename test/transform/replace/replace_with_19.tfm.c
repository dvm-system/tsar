void bar(double);
void foo(double);

void foo(double X) {
#pragma spf metadata replace(bar(X))
}

void bar(double X) {
  if (X > 0) {

    foo(X);
  }
}
