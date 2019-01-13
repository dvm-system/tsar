void f() {
  if (1)
    return;
  return;
}

void g() {
  #pragma spf transform inline
  f();
}
//CHECK: inline_if_8.c:9:3: remark: inline expansion of function call
//CHECK:   f();
//CHECK:   ^
//CHECK: inline_if_8.c:4:3: remark: remove unreachable code
//CHECK:   return;
//CHECK:   ^
