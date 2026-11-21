# RVV load/store edge case: whole-register load/store ignore vl (vl = 0 here)
# Run: kites-cli examples/vector_ldst_tests/whole_register.s --mem-dump 0x10000000 8
.text
    la       a0, src
    la       a1, out
    li       t1, 0
    vsetvli  t0, t1, e32, m1, ta, ma   # vl = 0
    vl1re32.v v1, (a0)                 # still moves a whole register (16 bytes)
    vs1r.v   v1, (a1)                  # out[0..3] = 1, 2, 3, 4
.data
out: .word 9, 9, 9, 9, 9
src: .word 1, 2, 3, 4, 5, 6, 7, 8
