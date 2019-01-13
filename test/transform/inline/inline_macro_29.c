#define DOT .
#define L [
#define R ]
#define EQ =
#define RANGE ...
#define _4 4
#define _6 6
#define _5 5

struct A {
  int X[10];
};

void f_DOT() {
  struct A A1 = { DOT X [ 4 ... 6 ] = 5 };
}

void f_L() {
  struct A A1 = { . X L 4 ... 6 ] = 5 };
}

void f_R() {
  struct A A1 = { . X [ 4 ... 6 R = 5 };
}

void f_EQ() {
  struct A A1 EQ { . X [ 4 ... 6 ] = 5 };
}

void f_RANGE() {
  struct A A1 = { . X [ 4 RANGE 6 ] = 5 };
}

void f_4() {
  struct A A1 = { . X [ _4 ... 6 ] = 5 };
}

void f_6() {
  struct A A1 = { . X [ 4 ... _6 ] = 5 };
}

void f_5() {
  struct A A1 = { . X [ 4 ... 6 ] = _5 };
}

void all() {
#pragma spf transform inline
{
  f_DOT();
  f_L();
  f_R();
  f_EQ();
  f_RANGE();
  f_4();
  f_6();
  f_5();
}
}
//CHECK: inline_macro_29.c:14:6: warning: disable inline expansion
//CHECK: void f_DOT() {
//CHECK:      ^
//CHECK: inline_macro_29.c:15:19: note: macro prevent inlining
//CHECK:   struct A A1 = { DOT X [ 4 ... 6 ] = 5 };
//CHECK:                   ^
//CHECK: inline_macro_29.c:1:13: note: expanded from macro 'DOT'
//CHECK: #define DOT .
//CHECK:             ^
//CHECK: inline_macro_29.c:18:6: warning: disable inline expansion
//CHECK: void f_L() {
//CHECK:      ^
//CHECK: inline_macro_29.c:19:23: note: macro prevent inlining
//CHECK:   struct A A1 = { . X L 4 ... 6 ] = 5 };
//CHECK:                       ^
//CHECK: inline_macro_29.c:2:11: note: expanded from macro 'L'
//CHECK: #define L [
//CHECK:           ^
//CHECK: inline_macro_29.c:22:6: warning: disable inline expansion
//CHECK: void f_R() {
//CHECK:      ^
//CHECK: inline_macro_29.c:23:33: note: macro prevent inlining
//CHECK:   struct A A1 = { . X [ 4 ... 6 R = 5 };
//CHECK:                                 ^
//CHECK: inline_macro_29.c:3:11: note: expanded from macro 'R'
//CHECK: #define R ]
//CHECK:           ^
//CHECK: inline_macro_29.c:26:6: warning: disable inline expansion
//CHECK: void f_EQ() {
//CHECK:      ^
//CHECK: inline_macro_29.c:27:15: note: macro prevent inlining
//CHECK:   struct A A1 EQ { . X [ 4 ... 6 ] = 5 };
//CHECK:               ^
//CHECK: inline_macro_29.c:4:9: note: expanded from here
//CHECK: #define EQ =
//CHECK:         ^
//CHECK: inline_macro_29.c:30:6: warning: disable inline expansion
//CHECK: void f_RANGE() {
//CHECK:      ^
//CHECK: inline_macro_29.c:31:27: note: macro prevent inlining
//CHECK:   struct A A1 = { . X [ 4 RANGE 6 ] = 5 };
//CHECK:                           ^
//CHECK: inline_macro_29.c:5:15: note: expanded from macro 'RANGE'
//CHECK: #define RANGE ...
//CHECK:               ^
//CHECK: inline_macro_29.c:34:6: warning: disable inline expansion
//CHECK: void f_4() {
//CHECK:      ^
//CHECK: inline_macro_29.c:35:25: note: macro prevent inlining
//CHECK:   struct A A1 = { . X [ _4 ... 6 ] = 5 };
//CHECK:                         ^
//CHECK: inline_macro_29.c:6:12: note: expanded from macro '_4'
//CHECK: #define _4 4
//CHECK:            ^
//CHECK: inline_macro_29.c:38:6: warning: disable inline expansion
//CHECK: void f_6() {
//CHECK:      ^
//CHECK: inline_macro_29.c:39:31: note: macro prevent inlining
//CHECK:   struct A A1 = { . X [ 4 ... _6 ] = 5 };
//CHECK:                               ^
//CHECK: inline_macro_29.c:7:12: note: expanded from macro '_6'
//CHECK: #define _6 6
//CHECK:            ^
//CHECK: inline_macro_29.c:42:6: warning: disable inline expansion
//CHECK: void f_5() {
//CHECK:      ^
//CHECK: inline_macro_29.c:43:37: note: macro prevent inlining
//CHECK:   struct A A1 = { . X [ 4 ... 6 ] = _5 };
//CHECK:                                     ^
//CHECK: inline_macro_29.c:8:12: note: expanded from macro '_5'
//CHECK: #define _5 5
//CHECK:            ^
//CHECK: 8 warnings generated.
