# Vector (RVV) demo for Kites.
# Run:  ./build/kites-cli examples/vector_demo.s
# Results: "vec_registers" in ~/Library/Application Support/Kites/vm_state/registers_dump.json

.text
    vsetivli t0, 4, e32, m1, ta, ma   # 4 elements of 32 bits; t0 = vl = 4
    li a0, 5
    vadd.vx v1, v0, a0                # v1 = [5, 5, 5, 5]
    vadd.vi v2, v1, 3                 # v2 = [8, 8, 8, 8]
    vsub.vv v3, v2, v1                # v3 = [3, 3, 3, 3]
    vxor.vi v4, v3, -1                # v4 = ~3 = [0xFFFFFFFC, ...]
    vwaddu.vv v6, v2, v3              # v6/v7 = 64-bit [11, 11, 11, 11]
    vsetivli t1, 4, e64, m1, ta, ma   # switch to 64-bit elements; t1 = vl = 2 (VLMAX)
    vsext.vf2 v8, v4                  # v8 = sign-extended [-4, -4]
