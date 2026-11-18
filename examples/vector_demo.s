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

# ---------------------------------------------------------------------------------------------
# Vector loads and stores (RVV 1.0 section 7). Uses only v0 (mask), v10-v28, s0, s1 and t2-t5,
# so everything above keeps its result. Check with:
#   kites-cli examples/vector_demo.s --mem-dump 0x10000000 38
# In registers_dump.json each vector register is one hex number, element 0 in the LOWEST
# (rightmost) bits. Stores land in "out" (0x10000090) in memory_dump.json.
# ---------------------------------------------------------------------------------------------
    lui  s0, 0x10000                  # s0 = 0x10000000 = start of .data (words)
    addi s1, s0, 144                  # s1 = 0x10000090 = out

    # 1. Unit-stride
    vsetivli x0, 4, e32, m1, ta, ma
    vle32.v  v10, (s0)                # v10 = [1, 2, 3, 4]
    vse32.v  v10, (s1)                # out+0  = 1, 2, 3, 4
    vsetivli x0, 16, e8, m1, ta, ma
    addi     t2, s0, 64
    vle8.v   v11, (t2)                # v11 = [11, 12, ..., 26]  (16 bytes)

    # 2. Strided (byte stride in a register; may be negative)
    vsetivli x0, 4, e32, m1, ta, ma
    li       t3, 8
    vlse32.v v12, (s0), t3            # every other word: v12 = [1, 3, 5, 7]
    addi     t2, s0, 28
    li       t4, -4
    vlse32.v v13, (t2), t4            # backwards: v13 = [8, 7, 6, 5]
    addi     t2, s1, 16
    vsse32.v v10, (t2), t3            # out+16 = 1, 0, 2, 0, 3, 0, 4, 0

    # 3. Indexed (index register holds byte offsets; data uses SEW)
    vsetivli  x0, 4, e8, m1, ta, ma
    addi      t2, s0, 128
    vle8.v    v14, (t2)               # v14 = byte offsets [28, 0, 20, 8]
    vsetivli  x0, 4, e32, m1, ta, ma
    vluxei8.v v15, (s0), v14          # gather (unordered): v15 = [8, 1, 6, 3]
    vloxei8.v v16, (s0), v14          # gather (ordered):   v16 = [8, 1, 6, 3]
    addi      t2, s1, 48
    vsoxei8.v v10, (t2), v14          # scatter: out+48 = 2, 0, 4, 0, 0, 3, 0, 1

    # 4. Mask load and masked load/store (v0 = 0b1101: elements 0, 2, 3 active)
    addi     t2, s0, 132
    vlm.v    v0, (t2)                 # v0 = 0x0d
    vle32.v  v17, (s0), v0.t          # v17 = [1, 0, 3, 4]  (element 1 left untouched)
    addi     t2, s1, 80
    vse32.v  v10, (t2), v0.t          # out+80 = 1, 0, 3, 4  (element 1 not written)

    # 5. Segment load/store: (x,y,z) points split across v18, v19, v20
    addi        t2, s0, 80
    vlseg3e32.v v18, (t2)             # v18 = x = [1, 4, 7, 10]
                                      # v19 = y = [2, 5, 8, 11]
                                      # v20 = z = [3, 6, 9, 12]
    addi        t2, s1, 96
    vsseg2e32.v v18, (t2)             # out+96 = 1,2, 4,5, 7,8, 10,11  (x,y interleaved)

    # 6. Whole-register load/store (ignores vtype and vl)
    vl2re32.v v22, (s0)               # v22 = [1, 2, 3, 4], v23 = [5, 6, 7, 8]
    addi      t2, s1, 128
    vs1r.v    v23, (t2)               # out+128 = 5, 6, 7, 8

    # 7. Fault-only-first: a fault after element 0 shrinks vl instead of trapping
    vsetivli  x0, 8, e8, m1, ta, ma
    li        t2, -4                  # 0xFFFF...FFFC: byte 0xFFFF...FFFF is past memory
    vle8ff.v  v24, (t2)               # elements 0-2 load (unwritten memory: v24 stays 0),
                                      # element 3 faults -> no trap, vl trimmed to 3
    csrrs     t5, vl, x0              # t5 = 3  (the proof: vl was 8)

    # 8. EEW different from SEW: e64 data with SEW=32 uses EMUL=2 (v26-v27)
    vsetivli  x0, 4, e32, m1, ta, ma
    addi      t2, s0, 32
    vle64.v   v26, (t2)               # v26 = [1000, 2000], v27 = [3000, 4000]

    # 9. vstart: elements below vstart are skipped, then vstart is reset to 0
    csrrwi    x0, vstart, 2
    vle32.v   v28, (s0)               # v28 = [0, 0, 3, 4]

.data
words:  .word 1, 2, 3, 4, 5, 6, 7, 8                                 # 0x10000000
dwords: .dword 1000, 2000, 3000, 4000                                # 0x10000020
bytes:  .byte 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26  # 0x10000040
points: .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12                  # 0x10000050 (x,y,z) x 4
offs:   .byte 28, 0, 20, 8                                           # 0x10000080
mask:   .byte 13                                                     # 0x10000084 = 0b1101
pad:    .zero 11                                                     # 0x10000085
out:    .zero 160                                                    # 0x10000090
