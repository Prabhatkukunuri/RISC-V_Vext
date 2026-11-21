# RVV load/store edge case: vle8ff.v: a fault after element 0 trims vl instead of trapping
# Run: kites-cli examples/vector_ldst_tests/fault_only_first.s --mem-dump 0x10000000 8
.text
    la       a1, out
    vsetivli x0, 8, e8, m1, ta, ma
    vmv.v.i  v1, 9                     # v1 = 9, 9, ..., 9
    li       a0, -4                    # 2^64 - 4: elements 0..2 valid, element 3 out of range
    vle8ff.v v1, (a0)                  # loads 3 bytes (0, 0, 0) and sets vl = 3
    vse8.v   v1, (a1)                  # stores only vl = 3 bytes: out = 00 00 00 77 77 ...
.data
out: .byte 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77
