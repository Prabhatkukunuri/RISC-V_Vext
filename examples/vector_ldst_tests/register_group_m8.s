# RVV load/store edge case: LMUL = 8: one load/store moves 32 words through v8-v15
# Run: kites-cli examples/vector_ldst_tests/register_group_m8.s --mem-dump 0x10000000 8
.text
    la       a0, src
    la       a1, out
    li       t1, 32
    vsetvli  t0, t1, e32, m8, ta, ma   # vl = 32
    vle32.v  v8, (a0)
    vse32.v  v8, (a1)
.data
out: .zero 128
src: .word 100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131
