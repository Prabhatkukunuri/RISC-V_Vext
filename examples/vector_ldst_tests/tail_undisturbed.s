# RVV load/store edge case: elements past vl are not loaded or stored
# Run: kites-cli examples/vector_ldst_tests/tail_undisturbed.s --mem-dump 0x10000000 8
.text
    la       a0, src
    la       a1, out
    vsetivli x0, 4, e32, m1, ta, ma
    vle32.v  v1, (a0)                  # v1 = 1, 2, 3, 4
    addi     a2, a0, 16
    vsetivli x0, 2, e32, m1, ta, ma
    vle32.v  v1, (a2)                  # vl = 2: v1 = 5, 6, 3, 4 (tail kept)
    vsetivli x0, 4, e32, m1, ta, ma
    vse32.v  v1, (a1)                  # out[0..3] = 5, 6, 3, 4
    addi     a3, a1, 16
    vsetivli x0, 3, e32, m1, ta, ma
    vse32.v  v1, (a3)                  # vl = 3: out[4..7] = 5, 6, 3, 9 (out[7] untouched)
.data
out: .word 0, 0, 0, 0, 9, 9, 9, 9
src: .word 1, 2, 3, 4, 5, 6, 7, 8
