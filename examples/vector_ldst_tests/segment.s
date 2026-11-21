# RVV load/store edge case: segment load/store: array of (x, y) pairs split and re-joined
# Run: kites-cli examples/vector_ldst_tests/segment.s --mem-dump 0x10000000 8
.text
    la       a0, pts
    la       a1, out
    vsetivli x0, 4, e32, m1, ta, ma
    vlseg2e32.v v4, (a0)               # v4 = x = 1, 2, 3, 4;  v5 = y = 10, 20, 30, 40
    vse32.v  v4, (a1)                  # out[0..3]  = x
    addi     a2, a1, 16
    vse32.v  v5, (a2)                  # out[4..7]  = y
    addi     a3, a1, 32
    vsseg2e32.v v4, (a3)               # out[8..15] = 1, 10, 2, 20, 3, 30, 4, 40
.data
out: .zero 64
pts: .word 1, 10, 2, 20, 3, 30, 4, 40
