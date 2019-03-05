!===--- Jacobi.f90 ----- Jacobi Iterative Method ----*- Fortran -*-===!
!
! This file implements Jacobi iterative method which is an iterative
! method used to solve partial differential equations.
!
!===---------------------------------------------------------------===!

program Jacobi
  parameter (L = 8, ITMAX = 10)
  real A(L,L), Eps, Maxeps, B(L, L)
  MAXEPS = 0.5
  do J = 1, L
    do I = 1, L
      A(I, J) = 0.
      if (I == 1 .or. J == 1 .or. I == L .or. J == L) then
        B(I, J) = 0.
      else
        B(I, J) = 1. + I + J
      endif
    enddo
  enddo
  do It = 1, ITMAX
    Eps = 0.
      do J = 2, L - 1
        do I = 2, L - 1
          Eps = max(Eps, abs(B(I, J) - A(I, J)))
          A(I, J) = B(I, J)
        enddo
      enddo
    do J = 2, L - 1
      do I = 2, l - 1
        B(I, J) =  (A(I, J-1) + A(I-1, J) + A(I+1, J) + A(I, J+1) ) / 4.
      enddo
    enddo
    print 200, It, Eps
200 format (' It = ', i4, '   Eps = ', e14.7)
    if (Eps < MAXEPS) exit
  enddo
end
