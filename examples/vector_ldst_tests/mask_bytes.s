# RVV load/store edge case: vlm.v / vsm.v move ceil(vl / 8) bytes (vl = 10 -> 2 bytes)
# Run: kites-cli examples/vector_ldst_tests/mask_bytes.s --mem-dump 0x10000000 8
.text
    la       a0, mbytes
    la       a1, out
    vsetivli x0, 10, e8, m1, ta, ma
    vlm.v    v0, (a0)                  # loads 2 bytes
    vsm.v    v0, (a1)                  # stores 2 bytes: out = A5 03 77 77
.data
out:    .byte 0x77, 0x77, 0x77, 0x77
mbytes: .byte 0xA5, 0x03, 0xFF, 0xFF
