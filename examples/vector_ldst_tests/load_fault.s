# RVV load/store edge case: an ordinary load that faults stops the run with an access fault
# Run: kites-cli examples/vector_ldst_tests/load_fault.s --mem-dump 0x10000000 8
.text
    vsetivli x0, 8, e8, m1, ta, ma
    li       a0, -4
    vle8.v   v1, (a0)                  # element 3 is out of range -> load access fault
.data
out: .word 0
