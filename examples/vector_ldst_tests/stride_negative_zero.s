# RVV load/store edge case: negative stride (reverse) and zero stride (broadcast)
# Run: kites-cli examples/vector_ldst_tests/stride_negative_zero.s --mem-dump 0x10000000 8
.text
    la       a0, src
    la       a1, out
    vsetivli x0, 4, e32, m1, ta, ma
    addi     a2, a0, 28                # &src[7]
    li       t0, -4
    vlse32.v v1, (a2), t0              # v1 = 8, 7, 6, 5
    vse32.v  v1, (a1)
    vlse32.v v2, (a0), zero            # stride 0: v2 = 1, 1, 1, 1
    addi     a3, a1, 16
    vse32.v  v2, (a3)
.data
out: .zero 32
src: .word 1, 2, 3, 4, 5, 6, 7, 8
