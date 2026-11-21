# RVV load/store edge case: indexed gather (vluxei32) and scatter (vsuxei32)
# Run: kites-cli examples/vector_ldst_tests/indexed.s --mem-dump 0x10000000 8
.text
    la       a0, src
    la       a1, out
    la       a2, idx
    vsetivli x0, 4, e32, m1, ta, ma
    vle32.v  v3, (a2)                  # byte offsets 12, 0, 8, 4
    vluxei32.v v1, (a0), v3            # gather: v1 = 4, 1, 3, 2
    vse32.v  v1, (a1)
    vle32.v  v4, (a0)                  # v4 = 1, 2, 3, 4
    addi     a3, a1, 16
    vsuxei32.v v4, (a3), v3            # scatter: out[4..7] = 2, 4, 3, 1
.data
out: .zero 32
src: .word 1, 2, 3, 4, 5, 6, 7, 8
idx: .word 12, 0, 8, 4
