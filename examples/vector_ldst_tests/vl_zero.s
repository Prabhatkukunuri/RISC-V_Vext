# RVV load/store edge case: vl = 0: loads and stores do nothing
# Run: kites-cli examples/vector_ldst_tests/vl_zero.s --mem-dump 0x10000000 8
.text
    li       t1, 0
    vsetvli  t0, t1, e32, m1, ta, ma   # AVL 0 -> vl = 0
    la       a0, src
    la       a1, out
    vle32.v  v1, (a0)                  # no element is loaded
    vse32.v  v1, (a1)                  # no element is stored: out keeps 7, 7, 7, 7
    sw       t0, 16(a1)                # out[4] = vl = 0
.data
out: .word 7, 7, 7, 7, 9
src: .word 1, 2, 3, 4, 5, 6, 7, 8
