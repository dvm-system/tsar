char bar();

char foo() {
#pragma spf transform propagate
  char C;
  return (C = bar()) && C;
}
//CHECK: propagate_18.c:6:25: warning: disable expression propagation
//CHECK:   return (C = bar()) && C;
//CHECK:                         ^
//CHECK: propagate_18.c:6:15: note: expression is not available at propagation point
//CHECK:   return (C = bar()) && C;
//CHECK:               ^
//CHECK: propagate_18.c:1:6: note: value may differs in definition and propagation points or may produce side effect
//CHECK: char bar();
//CHECK:      ^
//CHECK: 1 warning generated.
