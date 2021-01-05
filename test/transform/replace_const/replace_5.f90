subroutine foo()
  integer a(10), i; i = 0; 1 a(i+1) = i; i = i + 1; if (i < 10) goto 1;
end
!CHECK: 
