# RVV load/store edge case: scalar store then vector load, vector store then scalar load
# Run: kites-cli examples/vector_ldst_tests/scalar_vector_ordering.s --mem-dump 0x10000000 8
.text
    la       a0, src
    la       a1, out
    li       t0, 99
    sw       t0, 0(a0)                 # scalar store to src[0] ...
    vsetivli x0, 4, e32, m1, ta, ma
    vle32.v  v1, (a0)                  # ... must be seen by the next vector load: 99, 2, 3, 4
    vse32.v  v1, (a1)                  # vector store ...
    lw       t1, 0(a1)                 # ... must be seen by the next scalar load
    sw       t1, 16(a1)                # out[4] = 99
.data
out: .zero 20
src: .word 1, 2, 3, 4, 5, 6, 7, 8
