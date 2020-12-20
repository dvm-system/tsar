struct STy { int X; double Y; };

double foo(int N, struct STy S) {
  for (int I = 0; I < N; ++I) {
    S.Y += I;
    S.X *= I;
  }
  return S.Y + S.X;
}
//NOT_WINDOWS: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//NOT_WINDOWS:  loop at depth 1 struct_6.c:4:3
//NOT_WINDOWS:    induction:
//NOT_WINDOWS:     <I:4[4:3], 4>:[Int,0,,1]
//NOT_WINDOWS:    reduction:
//NOT_WINDOWS:     <S:3, 16>
//NOT_WINDOWS:    read only:
//NOT_WINDOWS:     <N:3, 4>
//NOT_WINDOWS:    lock:
//NOT_WINDOWS:     <I:4[4:3], 4> | <N:3, 4>
//NOT_WINDOWS:    header access:
//NOT_WINDOWS:     <I:4[4:3], 4> | <N:3, 4>
//NOT_WINDOWS:    explicit access:
//NOT_WINDOWS:     <I:4[4:3], 4> | <N:3, 4>
//NOT_WINDOWS:    explicit access (separate):
//NOT_WINDOWS:     <I:4[4:3], 4> <N:3, 4>
//NOT_WINDOWS:    lock (separate):
//NOT_WINDOWS:     <I:4[4:3], 4> <N:3, 4>
//NOT_WINDOWS:    direct access (separate):
//NOT_WINDOWS:     <I:4[4:3], 4> <N:3, 4> <S:3, 16>
//WINDOWS: Printing analysis 'Dependency Analysis (Metadata)' for function 'foo':
//WINDOWS:  loop at depth 1 struct_6.c:4:3
//WINDOWS:    output:
//WINDOWS:     <S:3:30, 16>
//WINDOWS:    anti:
//WINDOWS:     <S:3:30, 16>
//WINDOWS:    flow:
//WINDOWS:     <S:3:30, 16>
//WINDOWS:    induction:
//WINDOWS:     <I:4[4:3], 4>:[Int,0,,1]
//WINDOWS:    read only:
//WINDOWS:     <N:3, 4>
//WINDOWS:    lock:
//WINDOWS:     <I:4[4:3], 4> | <N:3, 4>
//WINDOWS:    header access:
//WINDOWS:     <I:4[4:3], 4> | <N:3, 4>
//WINDOWS:    explicit access:
//WINDOWS:     <I:4[4:3], 4> | <N:3, 4>
//WINDOWS:    explicit access (separate):
//WINDOWS:     <I:4[4:3], 4> <N:3, 4>
//WINDOWS:    lock (separate):
//WINDOWS:     <I:4[4:3], 4> <N:3, 4>
//WINDOWS:    direct access (separate):
//WINDOWS:     <I:4[4:3], 4> <N:3, 4> <S:3:30, 16>
