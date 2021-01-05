      subroutine foo
        implicit none
        integer i, l, u
        parameter(l = 100, u = 200)
        real a(l:u)
        common /d/ a
      integer(kind=4_8) il_0
      il_0 = l
        do i = 1, u - l
          a(i + il_0) = i
        enddo
      end
