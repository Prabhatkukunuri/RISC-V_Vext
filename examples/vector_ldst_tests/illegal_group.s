# RVV load/store edge case: misaligned register group is an illegal instruction
# Run: kites-cli examples/vector_ldst_tests/illegal_group.s --mem-dump 0x10000000 8
.text
    la       a0, out
    vsetivli x0, 4, e32, m2, ta, ma    # LMUL = 2: groups must start at an even register
    vle32.v  v1, (a0)                  # v1 is not aligned -> illegal
.data
out: .word 0
