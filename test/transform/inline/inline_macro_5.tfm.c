#define F(f_)                                                                  \
  void f_() {}

F(f1)

void f2() { f1(); }
