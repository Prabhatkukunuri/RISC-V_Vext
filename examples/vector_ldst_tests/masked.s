# RVV load/store edge case: masked load and masked store (v0 = 0b1010)
# Run: kites-cli examples/vector_ldst_tests/masked.s --mem-dump 0x10000000 8
.text
    la       a0, src
    la       a1, out
    la       a2, mask
    vsetivli x0, 4, e32, m1, ta, ma
    vlm.v    v0, (a2)                  # v0 = 0b1010: elements 1 and 3 active
    vle32.v  v1, (a0)                  # v1 = 1, 2, 3, 4
    vse32.v  v1, (a1), v0.t            # out[0..3] = 9, 2, 9, 4
    vmv.v.i  v2, 5                     # v2 = 5, 5, 5, 5
    vle32.v  v2, (a0), v0.t            # v2 = 5, 2, 5, 4
    addi     a3, a1, 16
    vse32.v  v2, (a3)                  # out[4..7] = 5, 2, 5, 4
.data
out:  .word 9, 9, 9, 9, 0, 0, 0, 0
src:  .word 1, 2, 3, 4, 5, 6, 7, 8
mask: .byte 10
