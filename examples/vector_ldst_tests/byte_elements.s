# RVV load/store edge case: 8-bit elements, and EEW (8) different from SEW (32)
# Run: kites-cli examples/vector_ldst_tests/byte_elements.s --mem-dump 0x10000000 8
.text
    la       a0, bytes
    la       a1, out
    vsetivli x0, 8, e8, m1, ta, ma
    vle8.v   v1, (a0)                  # 8 bytes
    vse8.v   v1, (a1)                  # out[0..7] = 0x11 .. 0x88
    vsetivli x0, 4, e32, m1, ta, ma    # SEW = 32, but vle8 uses EEW = 8 (EMUL = 1/4)
    addi     a2, a0, 4
    vle8.v   v2, (a2)                  # 4 bytes: 0x55 0x66 0x77 0x88
    addi     a3, a1, 8
    vse8.v   v2, (a3)                  # out[8..11]
.data
out:   .zero 16
bytes: .byte 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88
