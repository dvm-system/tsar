      subroutine bar(integer x)
        x = 5
      end

      module baz
        integer k
        parameter(k = 4)
        integer a(10, 2:5)
        contains
          subroutine foo(integer i)
      integer i1_0, i3_0, i4_0
      integer(kind=k) i3_k_0
      integer(kind=4_8) ik_0
      i1_0 = 1
      i3_0 = 3
      i4_0 = 4
      i3_k_0 = 3_k
      ik_0 = k
            call bar(a(i, i3_0))
            a(i,i3_k_0) = a(i + (-i1_0), i2_0) + a(ik_0,i4_0)
          end
        end
      end
