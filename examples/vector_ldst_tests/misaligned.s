# RVV load/store edge case: misaligned base addresses for a load and a store
# Run: kites-cli examples/vector_ldst_tests/misaligned.s --mem-dump 0x10000000 8
.text
    la       a0, src
    la       a1, out
    vsetivli x0, 4, e32, m1, ta, ma
    addi     a2, a0, 2
    vle32.v  v1, (a2)                  # load from src + 2 (not 4-byte aligned)
    vse32.v  v1, (a1)                  # aligned store
    addi     a3, a1, 21
    vse32.v  v1, (a3)                  # store to out + 21 (not aligned)
.data
out: .zero 40
src: .word 1, 2, 3, 4, 5, 6, 7, 8
