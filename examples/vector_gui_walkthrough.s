# Watch this run in the GUI with the Vector and Memory tabs open: set the speed box to
# e.g. 1000 ms, press Run, then Pause at any point and use Step / Undo / Redo / Resume.
# Expected results are written next to each instruction.

.data
a:    .word 1, 5, 3, 8, 2, 7, 4, 6        # 0x10000000
b:    .word 4, 4, 4, 4, 4, 4, 4, 4        # 0x10000020
rgb:  .byte 10, 20, 30, 11, 21, 31, 12, 22, 32, 13, 23, 33   # 0x10000040
offs: .byte 12, 0, 8, 4                   # 0x1000004C
out:  .zero 64                            # 0x10000050

.text
    lui      s0, 0x10000          # s0 = &a
    addi     s1, s0, 32           # s1 = &b
    addi     s2, s0, 80           # s2 = &out

    # Loads (section 7) with LMUL=2: v2-v3 and v4-v5 are register groups
    vsetivli t0, 8, e32, m2, ta, ma
    vle32.v  v2, (s0)             # v2..v3 = [1,5,3,8,2,7,4,6]
    vle32.v  v4, (s1)             # v4..v5 = [4,4,4,4,4,4,4,4]

    # Compare -> mask, then a masked add (sections 10 and 11)
    vmslt.vv v0, v2, v4           # v0 mask = 1 0 1 0 1 0 0 0   (a < b)
    vadd.vv  v6, v2, v4, v0.t     # v6..v7 = [5,0,7,0,6,0,0,0]

    # Widening and narrowing (section 10.2 / 10.3)
    vsetivli t0, 4, e32, m1, ta, ma
    vwadd.vv v8, v2, v4           # v8..v9 = e64 [5, 9, 7, 12]
    vnsrl.wi v10, v8, 0           # narrow e64 -> e32: v10 = [5, 9, 7, 12]

    # Strided, indexed and segment loads
    vsetivli t0, 4, e32, m1, ta, ma
    li       t1, 8
    vlse32.v v11, (s0), t1        # every other word: v11 = [1, 3, 2, 4]
    addi     t2, s0, 76
    vsetivli t0, 4, e8, m1, ta, ma
    vle8.v   v12, (t2)            # v12 = byte offsets [12, 0, 8, 4]
    vsetivli t0, 4, e32, m1, ta, ma
    vluxei8.v v13, (s0), v12      # gather a[3],a[0],a[2],a[1]: v13 = [8, 1, 3, 5]
    addi     t3, s0, 64
    vsetivli t0, 4, e8, m1, ta, ma
    vlseg3e8.v v14, (t3)          # R=v14 [10,11,12,13] G=v15 [20,..] B=v16 [30,..]

    # Permutations (section 16), on v11 = [1, 3, 2, 4] and v2 = [1, 5, 3, 8]
    vsetivli t0, 4, e32, m1, ta, ma
    vslideup.vi    v17, v11, 1    # v17 = [0, 1, 3, 2]  (element 0 left unchanged)
    vslidedown.vi  v18, v11, 1    # v18 = [3, 2, 4, 0]  (reads 0 past VLMAX)
    li             t6, 99
    vslide1up.vx   v19, v11, t6   # v19 = [99, 1, 3, 2]
    vslide1down.vx v20, v11, t6   # v20 = [3, 2, 4, 99]
    vrgather.vv    v21, v11, v2   # indices [1,5,3,8]: v21 = [3, 0, 4, 0] (5, 8 >= VLMAX -> 0)
    vrgather.vi    v26, v11, 3    # v26 = [4, 4, 4, 4]
    vcompress.vm   v22, v2, v0    # mask bits 1,0,1,0: v22 = [1, 3, 0, 0]
    vmv.x.s        a3, v20        # a3 = 3
    vmv.s.x        v23, a3        # v23 = [3, 0, 0, 0]
    vmv2r.v        v24, v2        # v24..v25 = v2..v3 (whole registers, ignores vl)

    # Stores: watch the Memory tab (click "Go to .data"); the bytes just written are highlighted
    vsetivli t0, 4, e32, m1, ta, ma
    vse32.v  v11, (s2)            # out[0..3]  = 1, 3, 2, 4
    addi     t4, s2, 16
    vsse32.v v13, (t4), t1        # out[4], out[6], out[8], out[10] = 8, 1, 3, 5
    addi     t5, s2, 50
    vse32.v  v13, (t5)            # misaligned (section 8): 16 bytes from 0x10000082
