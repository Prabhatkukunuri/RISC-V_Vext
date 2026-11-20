# Vector addition with RVV:  c[i] = a[i] + b[i]  for i = 0 .. N-1
#
# C equivalent:
#     for (int i = 0; i < n; i++)
#         c[i] = a[i] + b[i];
#
# The loop is strip-mined: each pass asks vsetvli how many elements fit
# (vl), processes vl elements at once, then advances the pointers.
# It works for any N and any VLEN.

.data
a:  .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
b:  .word 10, 20, 30, 40, 50, 60, 70, 80, 90, 100
c:  .word 0, 0, 0, 0, 0, 0, 0, 0, 0, 0     # result goes here

.text
    li    a0, 10              # n = number of elements
    la    a1, a               # pointer to a
    la    a2, b               # pointer to b
    la    a3, c               # pointer to c

loop:
    vsetvli t0, a0, e32, m1, ta, ma   # t0 = vl = min(n, VLMAX), 32-bit elements
    vle32.v v1, (a1)          # v1 = a[0 .. vl-1]
    vle32.v v2, (a2)          # v2 = b[0 .. vl-1]
    vadd.vv v3, v1, v2        # v3 = v1 + v2   (vl additions in one instruction)
    vse32.v v3, (a3)          # c[0 .. vl-1] = v3

    slli  t1, t0, 2           # bytes processed = vl * 4
    add   a1, a1, t1          # a += vl
    add   a2, a2, t1          # b += vl
    add   a3, a3, t1          # c += vl
    sub   a0, a0, t0          # n -= vl
    bnez  a0, loop            # repeat until all elements are done

# Expected c: 11, 22, 33, 44, 55, 66, 77, 88, 99, 110
