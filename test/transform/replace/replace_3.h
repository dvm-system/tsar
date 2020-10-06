void foo(struct STy *S) {
  #pragma spf transform replace (S)
  S->X = S->X + 1;
}
