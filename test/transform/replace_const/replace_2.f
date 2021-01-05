      subroutine foo
        implicit none
        integer i, l, u
        parameter(l = 100, u = 200)
        real a(l:u)
        common /d/ a
        do i = 1, u - l
          a(i + l) = i
        enddo
      end
!CHECK: 
