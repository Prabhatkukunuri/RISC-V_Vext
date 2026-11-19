# =================================================================================================
#  Every vector instruction Kites implements, one at a time (RVV 1.0, integer only: Zve64x).
#
#  Each line says what the instruction does; the value after "=>" is what Kites produced when
#  this file was run (element 0 first; mask results are shown one bit per element).
#
#  Run it:
#    GUI : open this file, set a step delay, then Step through it with the Vector and Memory
#          tabs open, comparing each register with the comment.
#    CLI : ./build/kites-cli examples/vector_all_instructions.s
#          (final state in ~/Library/Application Support/Kites/vm_state/registers_dump.json)
#
#  VLEN = 128 bits, so one vector register holds 16 x e8, 8 x e16, 4 x e32 or 2 x e64 elements.
#  Most of the program uses e32 with vl = 4: one full register per operand.
#
#  Sections follow the spec:   6 configuration     7 loads / stores     11 integer arithmetic
#                              14 reductions       15 mask              16 permutation
# =================================================================================================

.data
words:    .word  10, 20, 30, 40, 50, 60, 70, 80             # 0x10000000
small:    .word  1, 2, 3, 4                                 # 0x10000020
mixed:    .word  -5, 6, -7, 8                               # 0x10000030
bigs:     .word  0x40000000, 0x80000000, 0x7FFFFFFF, 0xFFFFFFFF   # 0x10000040
bytes:    .byte  1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16   # 0x10000050
sbytes:   .byte  -1, 2, -3, 4                               # 0x10000060
offsets:  .byte  12, 0, 8, 4                                # 0x10000064 (byte offsets)
halves:   .halfword 100, -200, 300, -400                       # 0x10000068
dwords:   .dword 0x100000005, -64, 0x7FFFFFFF0, 1024        # 0x10000070
maskbyte: .byte  13                                         # 0x10000090 = 0b00001101
masks:    .byte  180, 102                                   # 0x10000091 = 0b10110100, 0b01100110
pad:      .byte  0
rgb:      .byte  10, 20, 30, 11, 21, 31, 12, 22, 32, 13, 23, 33   # 0x10000094: 4 RGB pixels
pad2:     .zero  16
out:      .zero  256                                        # 0x100000b0: stores go here

.text
# ------------------------------------------------------------------------------------- setup
    la    s0, words
    la    s1, bytes
    la    s2, halves
    la    s3, dwords
    la    s4, offsets
    la    s5, maskbyte
    la    s6, rgb
    la    s7, out
    li    a0, 3
    li    a1, -2
    li    a2, 100

# =================================================================================================
# 6  CONFIGURATION: vl = how many elements, vtype = element width (SEW), grouping (LMUL), policy
# =================================================================================================
    vsetvli  t0, a2, e32, m1, ta, ma     # ask for 100 elements of 32 bits; VLMAX = 128/32 = 4  => t0 = 4; vl = 4, vtype = e32, m1, ta, ma
    vsetvli  t0, x0, e8, m1, ta, ma      # rs1 = x0: "as many as fit", vl = VLMAX = 16  => t0 = 16; vl = 16, vtype = e8, m1, ta, ma
    vsetvli  x0, x0, e16, m1, ta, ma     # rd = rs1 = x0: keep vl, change vtype (capped to 8)  => vl = 8, vtype = e16, m1, ta, ma
    vsetivli t0, 3, e32, m2, tu, mu      # AVL as an immediate; LMUL = 2 groups 2 registers  => t0 = 3; vl = 3, vtype = e32, m2, tu, mu
    li       t1, 0xd0                    # 0xd0 is the vtype bits for e32, m1, ta, ma
    vsetvl   t0, a2, t1                  # vtype taken from a register  => t0 = 4; vl = 4, vtype = e32, m1, ta, ma
    csrrs    t2, vlenb, x0               # read CSR vlenb = VLEN / 8 bytes  => t2 = 16

# =================================================================================================
# 7  LOADS AND STORES: every addressing mode is shown once. Each also exists for the other
#    element widths (e8/e16/e32/e64, e.g. vlse8.v, vluxei32.v, vle16ff.v), segment forms for
#    2 to 8 fields (vlseg2e8.v ... vlseg8e64.v), and whole-register forms for 1/2/4/8 registers.
# =================================================================================================
# -- unit-stride: consecutive elements
    vle32.v   v1, (s0)                   # load 4 words from words[0]  => v1 = [10, 20, 30, 40]
    vse32.v   v1, (s7)                   # store them to out+0  => mem 0x100000b0=10, 0x100000b4=20, 0x100000b8=30, 0x100000bc=40
    vsetivli  x0, 8, e8, m1, ta, ma   # => vl = 8, vtype = e8, m1, ta, ma
    vle8.v    v2, (s1)                   # load 8 bytes  => v2 = [1, 2, 3, 4, 5, 6, 7, 8]
    addi      t3, s7, 16
    vse8.v    v2, (t3)                   # store 8 bytes to out+16  => mem 0x100000c0=1, 0x100000c1=2, 0x100000c2=3, 0x100000c3=4, 0x100000c4=5, 0x100000c5=6, 0x100000c6=7, 0x100000c7=8
    vsetivli  x0, 4, e16, m1, ta, ma   # => vl = 4, vtype = e16, m1, ta, ma
    vle16.v   v3, (s2)                   # load 4 halfwords  => v3 = [100, -200, 300, -400]
    addi      t3, s7, 24
    vse16.v   v3, (t3)                   # store 4 halfwords to out+24  => mem 0x100000c8=100, 0x100000ca=-200, 0x100000cc=300, 0x100000ce=-400
    vsetivli  x0, 2, e64, m1, ta, ma   # => vl = 2, vtype = e64, m1, ta, ma
    vle64.v   v4, (s3)                   # load 2 doublewords  => v4 = [4294967301, -64]
    addi      t3, s7, 32
    vse64.v   v4, (t3)                   # store 2 doublewords to out+32  => mem 0x100000d0=4294967301, 0x100000d8=-64
# -- mask load / store: one bit per element, ceil(vl / 8) bytes
    vsetivli  x0, 8, e8, m1, ta, ma   # => vl = 8, vtype = e8, m1, ta, ma
    vlm.v     v0, (s5)                   # v0 = mask from byte 13 = 0b00001101  => v0 mask [1 0 1 1 0 0 0 0]
    addi      t3, s7, 48
    vsm.v     v0, (t3)                   # store the mask byte to out+48  => mem 0x100000e0=13
# -- strided: element i at base + i * stride
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma
    li        t1, 8
    vlse32.v  v5, (s0), t1               # every other word (stride 8 bytes)  => v5 = [10, 30, 50, 70]
    addi      t3, s7, 56
    vsse32.v  v5, (t3), t1               # store them 8 bytes apart from out+56  => mem 0x100000e8=10, 0x100000f0=30, 0x100000f8=50, 0x10000100=70
# -- indexed: element i at base + index[i] (byte offsets held in a vector register)
    vsetivli  x0, 4, e8, m1, ta, ma   # => vl = 4, vtype = e8, m1, ta, ma
    vle8.v    v6, (s4)                   # offsets = [12, 0, 8, 4]  => v6 = [12, 0, 8, 4]
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma
    vluxei8.v v7, (s0), v6               # gather words[3], [0], [2], [1] (unordered)  => v7 = [40, 10, 30, 20]
    vloxei8.v v8, (s0), v6               # the same, done in element order (ordered)  => v8 = [40, 10, 30, 20]
    addi      t3, s7, 88
    vsuxei8.v v1, (t3), v6               # scatter v1 to out+88 + offsets (unordered)  => mem 0x10000108=20, 0x1000010c=40, 0x10000110=30, 0x10000114=10
    addi      t3, s7, 104
    vsoxei8.v v5, (t3), v6               # scatter v5 in element order (ordered)  => mem 0x10000118=30, 0x1000011c=70, 0x10000120=50, 0x10000124=10
# -- fault-only-first: like vle, but a fault after element 0 only shortens vl
    vle32ff.v v9, (s0)                   # no fault here, so vl stays 4  => v9 = [10, 20, 30, 40]
# -- segment: NFIELDS consecutive fields per element, split across NFIELDS registers
    vsetivli  x0, 4, e8, m1, ta, ma   # => vl = 4, vtype = e8, m1, ta, ma
    vlseg3e8.v  v10, (s6)                # RGB pixels: v10 = R, v11 = G, v12 = B  => v10 = [10, 11, 12, 13]; v11 = [20, 21, 22, 23]; v12 = [30, 31, 32, 33]
    addi      t3, s7, 120
    vsseg3e8.v  v10, (t3)                # interleave them again at out+120  => mem 0x10000128=10, 0x10000129=20, 0x1000012a=30, 0x1000012b=11, 0x1000012c=21, 0x1000012d=31, 0x1000012e=12, 0x1000012f=22, 0x10000130=32, 0x10000131=13, 0x10000132=23, 0x10000133=33
    vsetivli  x0, 2, e32, m1, ta, ma   # => vl = 2, vtype = e32, m1, ta, ma
    li        t1, 16
    vlsseg2e32.v v13, (s0), t1           # strided segments: word pairs 16 bytes apart  => v13 = [10, 50]; v14 = [20, 60]
    addi      t3, s7, 136
    vssseg2e32.v v13, (t3), t1           # store the pairs 16 bytes apart  => mem 0x10000138=10, 0x1000013c=20, 0x10000148=50, 0x1000014c=60
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma
    vluxseg2ei8.v v15, (s0), v6          # indexed segments: word pairs at the byte offsets  => v15 = [40, 10, 30, 20]; v16 = [50, 20, 40, 30]
    vloxseg2ei8.v v17, (s0), v6          # the same, ordered  => v17 = [40, 10, 30, 20]; v18 = [50, 20, 40, 30]
    addi      t3, s7, 160
    vsuxseg2ei8.v v15, (t3), v6          # scatter the pairs (unordered)  => mem 0x10000150=10, 0x10000154=20, 0x10000158=30, 0x1000015c=40, 0x10000160=50
    addi      t3, s7, 184
    vsoxseg2ei8.v v17, (t3), v6          # scatter the pairs (ordered)  => mem 0x10000168=10, 0x1000016c=20, 0x10000170=30, 0x10000174=40, 0x10000178=50
# -- whole register: ignores vl and vtype, moves NREG x VLEN bits
    vl1re32.v v18, (s0)                  # one whole register: 4 words  => v18 = [10, 20, 30, 40]
    vl2re32.v v20, (s0)                  # two registers v20-v21: 8 words  => v20 = [10, 20, 30, 40]; v21 = [50, 60, 70, 80]
    addi      t3, s7, 208
    vs1r.v    v18, (t3)                  # store one register (16 bytes)  => mem 0x10000180=10, 0x10000184=20, 0x10000188=30, 0x1000018c=40
    addi      t3, s7, 224
    vs2r.v    v20, (t3)                  # store two registers (32 bytes)  => mem 0x10000190=10, 0x10000194=20, 0x10000198=30, 0x1000019c=40, 0x100001a0=50, 0x100001a4=60, 0x100001a8=70, 0x100001ac=80

# =================================================================================================
# 11  INTEGER ARITHMETIC (e32, vl = 4). Operands used throughout:
#     v1 = [10, 20, 30, 40]   v2 = [1, 2, 3, 4]   v3 = [-5, 6, -7, 8]   a0 = 3   a1 = -2
#     .vv = vector-vector   .vx = vector-scalar register   .vi = vector-immediate (5 bits)
# =================================================================================================
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma
    vle32.v   v1, (s0)                   # v1  => v1 = [10, 20, 30, 40]
    addi      t3, s0, 32
    vle32.v   v2, (t3)                   # v2 (small)  => v2 = [1, 2, 3, 4]
    addi      t3, s0, 48
    vle32.v   v3, (t3)                   # v3 (mixed)  => v3 = [-5, 6, -7, 8]

# -- 11.1 add / subtract
    vadd.vv   v8, v1, v2                 # v1 + v2  => v8 = [11, 22, 33, 44]
    vadd.vx   v8, v1, a0                 # v1 + 3  => v8 = [13, 23, 33, 43]
    vadd.vi   v8, v1, -1                 # v1 + (-1)  => v8 = [9, 19, 29, 39]
    vsub.vv   v8, v1, v2                 # v1 - v2  => v8 = [9, 18, 27, 36]
    vsub.vx   v8, v1, a0                 # v1 - 3  => v8 = [7, 17, 27, 37]
    vrsub.vx  v8, v1, a0                 # 3 - v1   (reverse subtract)  => v8 = [-7, -17, -27, -37]
    vrsub.vi  v8, v1, 0                  # 0 - v1   (negate)  => v8 = [-10, -20, -30, -40]

# -- 11.2 widening add / subtract: results are 2*SEW = 64 bits, in the group v8-v9
#    u = operands zero-extended, no u = sign-extended; .w* = vs2 is already wide
    vwaddu.vv v8, v2, v3                 # 1 + 0xFFFFFFFB, ... (v3 read as unsigned)  => v8 = [4294967292, 8, 4294967292, 12] (64-bit)
    vwadd.vv  v8, v2, v3                 # 1 + (-5), ...       (v3 read as signed)  => v8 = [-4, 8, -4, 12] (64-bit)
    vwsubu.vv v8, v2, v3                 # unsigned v2 - v3  => v8 = [-4294967290, -4, -4294967286, -4] (64-bit)
    vwsub.vv  v8, v2, v3                 # signed v2 - v3  => v8 = [6, -4, 10, -4] (64-bit)
    vwaddu.vx v8, v2, a1                 # v2 + 0xFFFFFFFE  => v8 = [4294967295, 4294967296, 4294967297, 4294967298] (64-bit)
    vwadd.vx  v8, v2, a1                 # v2 + (-2)  => v8 = [-1, 0, 1, 2] (64-bit)
    vwsubu.vx v8, v2, a1                 # v2 - 0xFFFFFFFE  => v8 = [-4294967293, -4294967292, -4294967291, -4294967290] (64-bit)
    vwsub.vx  v8, v2, a1                 # v2 - (-2)  => v8 = [3, 4, 5, 6] (64-bit)
    vwadd.vv  v10, v1, v1                # make a wide operand: v10-v11 = [20, 40, 60, 80] (e64)  => v10 = [20, 40, 60, 80] (64-bit)
    vwaddu.wv v8, v10, v3                # wide + zero-extended v3  => v8 = [4294967311, 46, 4294967349, 88] (64-bit)
    vwadd.wv  v8, v10, v3                # wide + sign-extended v3  => v8 = [15, 46, 53, 88] (64-bit)
    vwsubu.wv v8, v10, v3                # wide - zero-extended v3  => v8 = [-4294967271, 34, -4294967229, 72] (64-bit)
    vwsub.wv  v8, v10, v3                # wide - sign-extended v3  => v8 = [25, 34, 67, 72] (64-bit)
    vwaddu.wx v8, v10, a1                # wide + 0xFFFFFFFE  => v8 = [4294967314, 4294967334, 4294967354, 4294967374] (64-bit)
    vwadd.wx  v8, v10, a1                # wide + (-2)  => v8 = [18, 38, 58, 78] (64-bit)
    vwsubu.wx v8, v10, a1                # wide - 0xFFFFFFFE  => v8 = [-4294967274, -4294967254, -4294967234, -4294967214] (64-bit)
    vwsub.wx  v8, v10, a1                # wide - (-2)  => v8 = [22, 42, 62, 82] (64-bit)

# -- 11.3 extension: vd is SEW, vs2 is SEW/2 (vf2), SEW/4 (vf4) or SEW/8 (vf8)
    vsetivli  x0, 4, e16, m1, ta, ma   # => vl = 4, vtype = e16, m1, ta, ma
    vle16.v   v12, (s2)                  # e16 source [100, -200, 300, -400]  => v12 = [100, -200, 300, -400]
    vsetivli  x0, 4, e8, m1, ta, ma   # => vl = 4, vtype = e8, m1, ta, ma
    la        t3, sbytes
    vle8.v    v13, (t3)                  # e8 source [-1, 2, -3, 4]  => v13 = [-1, 2, -3, 4]
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma
    vzext.vf2 v8, v12                    # e16 -> e32, zero-extend  => v8 = [100, 65336, 300, 65136]
    vsext.vf2 v8, v12                    # e16 -> e32, sign-extend  => v8 = [100, -200, 300, -400]
    vzext.vf4 v8, v13                    # e8 -> e32, zero-extend  => v8 = [255, 2, 253, 4]
    vsext.vf4 v8, v13                    # e8 -> e32, sign-extend  => v8 = [-1, 2, -3, 4]
    vsetivli  x0, 2, e64, m1, ta, ma   # => vl = 2, vtype = e64, m1, ta, ma
    vzext.vf8 v8, v13                    # e8 -> e64, zero-extend  => v8 = [255, 2]
    vsext.vf8 v8, v13                    # e8 -> e64, sign-extend  => v8 = [-1, 2]
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma

# -- 11.4 add-with-carry / subtract-with-borrow (carry-in / borrow-in comes from v0)
#    v0 = mask [1 0 1 1] (loaded above); operands are unsigned SEW values
    vadc.vvm  v8, v3, v3, v0             # v3 + v3 + v0.mask  => v8 = [-9, 12, -13, 17]
    vadc.vxm  v8, v3, a1, v0             # v3 + (-2) + v0.mask  => v8 = [-6, 4, -8, 7]
    vadc.vim  v8, v3, 5, v0              # v3 + 5 + v0.mask  => v8 = [1, 11, -1, 14]
    vmadc.vvm v8, v3, v3, v0             # carry out of v3 + v3 + v0.mask  => v8 mask [1 0 1 0]
    vmadc.vxm v8, v3, a1, v0             # carry out of v3 + 0xFFFFFFFE + v0.mask  => v8 mask [1 1 1 1]
    vmadc.vim v8, v3, 5, v0              # carry out of v3 + 5 + v0.mask  => v8 mask [1 0 0 0]
    vmadc.vv  v8, v3, v3                 # carry out of v3 + v3 (no carry-in)  => v8 mask [1 0 1 0]
    vmadc.vx  v8, v3, a1                 # carry out of v3 + 0xFFFFFFFE  => v8 mask [1 1 1 1]
    vmadc.vi  v8, v3, 5                  # carry out of v3 + 5  => v8 mask [1 0 0 0]
    vsbc.vvm  v8, v2, v3, v0             # v2 - v3 - v0.mask  => v8 = [5, -4, 9, -5]
    vsbc.vxm  v8, v1, a0, v0             # v1 - 3 - v0.mask  => v8 = [6, 17, 26, 36]
    vmsbc.vvm v8, v2, v3, v0             # borrow out of v2 - v3 - v0.mask (unsigned)  => v8 mask [1 1 1 1]
    vmsbc.vxm v8, v2, a0, v0             # borrow out of v2 - 3 - v0.mask  => v8 mask [1 1 1 0]
    vmsbc.vv  v8, v1, v3                 # borrow out of v1 - v3 (no borrow-in)  => v8 mask [1 0 1 0]
    vmsbc.vx  v8, v2, a0                 # borrow out of v2 - 3  => v8 mask [1 1 0 0]

# -- 11.5 bitwise logical
    vand.vv   v8, v1, v3                 # v1 & v3  => v8 = [10, 4, 24, 8]
    vand.vx   v8, v1, a0                 # v1 & 3  => v8 = [2, 0, 2, 0]
    vand.vi   v8, v1, 12                 # v1 & 12  => v8 = [8, 4, 12, 8]
    vor.vv    v8, v1, v2                 # v1 | v2  => v8 = [11, 22, 31, 44]
    vor.vx    v8, v1, a0                 # v1 | 3  => v8 = [11, 23, 31, 43]
    vor.vi    v8, v1, 5                  # v1 | 5  => v8 = [15, 21, 31, 45]
    vxor.vv   v8, v1, v2                 # v1 ^ v2  => v8 = [11, 22, 29, 44]
    vxor.vx   v8, v1, a1                 # v1 ^ -2  => v8 = [-12, -22, -32, -42]
    vxor.vi   v8, v1, -1                 # v1 ^ -1 = NOT v1  => v8 = [-11, -21, -31, -41]

# -- 11.6 single-width shifts (shift amount uses the low log2(SEW) = 5 bits)
    vsll.vv   v8, v2, v2                 # v2 << v2 = [1<<1, 2<<2, 3<<3, 4<<4]  => v8 = [2, 8, 24, 64]
    vsll.vx   v8, v1, a0                 # v1 << 3  => v8 = [80, 160, 240, 320]
    vsll.vi   v8, v1, 4                  # v1 << 4  => v8 = [160, 320, 480, 640]
    vsrl.vv   v8, v3, v2                 # logical right shift: fills with 0  => v8 = [2147483645, 1, 536870911, 0]
    vsrl.vx   v8, v3, a0                 # v3 >> 3 (logical)  => v8 = [536870911, 0, 536870911, 1]
    vsrl.vi   v8, v3, 28                 # v3 >> 28 (logical)  => v8 = [15, 0, 15, 0]
    vsra.vv   v8, v3, v2                 # arithmetic right shift: fills with the sign  => v8 = [-3, 1, -1, 0]
    vsra.vx   v8, v3, a0                 # v3 >> 3 (arithmetic)  => v8 = [-1, 0, -1, 1]
    vsra.vi   v8, v3, 1                  # v3 >> 1 (arithmetic)  => v8 = [-3, 3, -4, 4]

# -- 11.7 narrowing shifts: vs2 is 2*SEW wide, the result is SEW
    vsetivli  x0, 4, e64, m2, ta, ma   # => vl = 4, vtype = e64, m2, ta, ma
    vle64.v   v10, (s3)                  # wide source v10-v11 = dwords (e64)  => v10 = [4294967301, -64, 34359738352, 1024]
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma
    vnsrl.wi  v8, v10, 0                 # just the low 32 bits of each 64-bit element  => v8 = [5, -64, -16, 1024]
    vnsrl.wi  v8, v10, 4                 # (wide >> 4), logical, then cut to 32 bits  => v8 = [268435456, -4, 2147483647, 64]
    li        t1, 60
    vnsrl.wx  v8, v10, t1                # (wide >> 60), logical: zeros shift in  => v8 = [0, 15, 0, 0]
    vnsrl.wv  v8, v10, v2                # (wide >> v2[i]), logical  => v8 = [-2147483646, -16, -2, 64]
    vnsra.wi  v8, v10, 4                 # (wide >> 4), arithmetic  => v8 = [268435456, -4, 2147483647, 64]
    vnsra.wx  v8, v10, t1                # (wide >> 60), arithmetic: copies of the sign shift in  => v8 = [0, -1, 0, 0]
    vnsra.wv  v8, v10, v2                # (wide >> v2[i]), arithmetic  => v8 = [-2147483646, -16, -2, 64]

# -- 11.8 compares: the result is a MASK (one bit per element)
    vmseq.vv  v8, v1, v1                 # ==  => v8 mask [1 1 1 1]
    vmseq.vx  v8, v2, a0                 # v2 == 3  => v8 mask [0 0 1 0]
    vmseq.vi  v8, v2, 4                  # v2 == 4  => v8 mask [0 0 0 1]
    vmsne.vv  v8, v2, v3                 # !=  => v8 mask [1 1 1 1]
    vmsne.vx  v8, v2, a0                 # v2 != 3  => v8 mask [1 1 0 1]
    vmsne.vi  v8, v2, 4                  # v2 != 4  => v8 mask [1 1 1 0]
    vmsltu.vv v8, v3, v2                 # unsigned <  (v3's negatives are huge)  => v8 mask [0 0 0 0]
    vmsltu.vx v8, v2, a0                 # unsigned v2 < 3  => v8 mask [1 1 0 0]
    vmslt.vv  v8, v3, v2                 # signed <  => v8 mask [1 0 1 0]
    vmslt.vx  v8, v3, a1                 # signed v3 < -2  => v8 mask [1 0 1 0]
    vmsleu.vv v8, v2, v2                 # unsigned <=  => v8 mask [1 1 1 1]
    vmsleu.vx v8, v2, a0                 # unsigned v2 <= 3  => v8 mask [1 1 1 0]
    vmsleu.vi v8, v2, 2                  # unsigned v2 <= 2  => v8 mask [1 1 0 0]
    vmsle.vv  v8, v3, v2                 # signed <=  => v8 mask [1 0 1 0]
    vmsle.vx  v8, v3, a1                 # signed v3 <= -2  => v8 mask [1 0 1 0]
    vmsle.vi  v8, v3, 6                  # signed v3 <= 6  => v8 mask [1 1 1 0]
    vmsgtu.vx v8, v3, a0                 # unsigned v3 > 3  => v8 mask [1 1 1 1]
    vmsgtu.vi v8, v2, 2                  # unsigned v2 > 2  => v8 mask [0 0 1 1]
    vmsgt.vx  v8, v3, a1                 # signed v3 > -2  => v8 mask [0 1 0 1]
    vmsgt.vi  v8, v3, 0                  # signed v3 > 0  => v8 mask [0 1 0 1]
#    assembler aliases (no encoding of their own; the assembler rewrites them):
    vmsgt.vv  v8, v3, v2                 # -> vmslt.vv v8, v2, v3   (operands swapped)  => v8 mask [0 1 0 1]
    vmsgtu.vv v8, v3, v2                 # -> vmsltu.vv v8, v2, v3  => v8 mask [1 1 1 1]
    vmsge.vv  v8, v3, v2                 # -> vmsle.vv v8, v2, v3  => v8 mask [0 1 0 1]
    vmsgeu.vv v8, v3, v2                 # -> vmsleu.vv v8, v2, v3  => v8 mask [1 1 1 1]
    vmslt.vi  v8, v2, 3                  # -> vmsle.vi v8, v2, 2    (immediate - 1)  => v8 mask [1 1 0 0]
    vmsltu.vi v8, v2, 3                  # -> vmsleu.vi v8, v2, 2  => v8 mask [1 1 0 0]
    vmsge.vi  v8, v3, 0                  # -> vmsgt.vi v8, v3, -1  => v8 mask [0 1 0 1]
    vmsgeu.vi v8, v2, 3                  # -> vmsgtu.vi v8, v2, 2  => v8 mask [0 0 1 1]

# -- 11.9 min / max
    vminu.vv  v8, v2, v3                 # unsigned min  => v8 = [1, 2, 3, 4]
    vminu.vx  v8, v3, a0                 # unsigned min(v3, 3)  => v8 = [3, 3, 3, 3]
    vmin.vv   v8, v2, v3                 # signed min  => v8 = [-5, 2, -7, 4]
    vmin.vx   v8, v3, a1                 # signed min(v3, -2)  => v8 = [-5, -2, -7, -2]
    vmaxu.vv  v8, v2, v3                 # unsigned max  => v8 = [-5, 6, -7, 8]
    vmaxu.vx  v8, v2, a0                 # unsigned max(v2, 3)  => v8 = [3, 3, 3, 4]
    vmax.vv   v8, v2, v3                 # signed max  => v8 = [1, 6, 3, 8]
    vmax.vx   v8, v3, a1                 # signed max(v3, -2)  => v8 = [-2, 6, -2, 8]

# -- 11.10 multiply: vmul keeps the low 32 bits, vmulh* the high 32 bits of the 64-bit product
    addi      t3, s0, 64
    vle32.v   v4, (t3)                   # v4 = bigs [2^30, -2^31, 2^31-1, -1]  => v4 = [1073741824, -2147483648, 2147483647, -1]
    vmul.vv   v8, v1, v2                 # low half of v1 * v2  => v8 = [10, 40, 90, 160]
    vmul.vx   v8, v1, a1                 # low half of v1 * -2  => v8 = [-20, -40, -60, -80]
    vmulh.vv  v8, v4, v4                 # high half, signed x signed  => v8 = [268435456, 1073741824, 1073741823, 0]
    vmulh.vx  v8, v4, a1                 # high half of v4 * -2  => v8 = [-1, 1, -1, 0]
    vmulhu.vv v8, v4, v4                 # high half, unsigned x unsigned  => v8 = [268435456, 1073741824, 1073741823, -2]
    vmulhu.vx v8, v4, a1                 # high half of v4 * 0xFFFFFFFE (unsigned)  => v8 = [1073741823, 2147483647, 2147483646, -3]
    vmulhsu.vv v8, v4, v4                # high half, signed v4 x unsigned v4  => v8 = [268435456, -1073741824, 1073741823, -1]
    vmulhsu.vx v8, v4, a1                # high half, signed v4 x unsigned 0xFFFFFFFE  => v8 = [1073741823, -2147483647, 2147483646, -1]

# -- 11.11 divide / remainder (x / 0 = all ones, x % 0 = x, like scalar RISC-V)
    vdivu.vv  v8, v1, v2                 # unsigned v1 / v2  => v8 = [10, 10, 10, 10]
    vdivu.vx  v8, v3, a0                 # unsigned v3 / 3  => v8 = [1431655763, 2, 1431655763, 2]
    vdiv.vv   v8, v3, v2                 # signed v3 / v2 (rounds toward zero)  => v8 = [-5, 3, -2, 2]
    vdiv.vx   v8, v1, x0                 # divide by zero -> -1  => v8 = [-1, -1, -1, -1]
    vremu.vv  v8, v1, v3                 # unsigned remainder  => v8 = [10, 2, 30, 0]
    vremu.vx  v8, v1, a0                 # unsigned v1 % 3  => v8 = [1, 2, 0, 1]
    vrem.vv   v8, v3, v2                 # signed remainder (takes the dividend's sign)  => v8 = [0, 0, -1, 0]
    vrem.vx   v8, v1, x0                 # remainder by zero -> v1  => v8 = [10, 20, 30, 40]

# -- 11.12 widening multiply: the full 64-bit product in v8-v9
    vwmul.vv  v8, v3, v2                 # signed x signed  => v8 = [-5, 12, -21, 32] (64-bit)
    vwmul.vx  v8, v3, a1                 # signed v3 x -2  => v8 = [10, -12, 14, -16] (64-bit)
    vwmulu.vv v8, v3, v2                 # unsigned x unsigned  => v8 = [4294967291, 12, 12884901867, 32] (64-bit)
    vwmulu.vx v8, v2, a1                 # unsigned v2 x 0xFFFFFFFE  => v8 = [4294967294, 8589934588, 12884901882, 17179869176] (64-bit)
    vwmulsu.vv v8, v3, v2                # signed v3 x unsigned v2  => v8 = [-5, 12, -21, 32] (64-bit)
    vwmulsu.vx v8, v3, a1                # signed v3 x unsigned 0xFFFFFFFE  => v8 = [-21474836470, 25769803764, -30064771058, 34359738352] (64-bit)

# -- 11.13 multiply-add: vd is also an input. Assembly order: vd, vs1/rs1, vs2
    vmv.v.v   v8, v1                     # v8 = v1 (the accumulator)  => v8 = [10, 20, 30, 40]
    vmacc.vv  v8, v2, v3                 # v8 = v8 + v2 * v3  => v8 = [5, 32, 9, 72]
    vmv.v.v   v8, v1   # => v8 = [10, 20, 30, 40]
    vmacc.vx  v8, a0, v3                 # v8 = v8 + 3 * v3  => v8 = [-5, 38, 9, 64]
    vmv.v.v   v8, v1   # => v8 = [10, 20, 30, 40]
    vnmsac.vv v8, v2, v3                 # v8 = v8 - v2 * v3  => v8 = [15, 8, 51, 8]
    vmv.v.v   v8, v1   # => v8 = [10, 20, 30, 40]
    vnmsac.vx v8, a0, v3                 # v8 = v8 - 3 * v3  => v8 = [25, 2, 51, 16]
    vmv.v.v   v8, v1   # => v8 = [10, 20, 30, 40]
    vmadd.vv  v8, v2, v3                 # v8 = v8 * v2 + v3  => v8 = [5, 46, 83, 168]
    vmv.v.v   v8, v1   # => v8 = [10, 20, 30, 40]
    vmadd.vx  v8, a0, v3                 # v8 = v8 * 3 + v3  => v8 = [25, 66, 83, 128]
    vmv.v.v   v8, v1   # => v8 = [10, 20, 30, 40]
    vnmsub.vv v8, v2, v3                 # v8 = -(v8 * v2) + v3  => v8 = [-15, -34, -97, -152]
    vmv.v.v   v8, v1   # => v8 = [10, 20, 30, 40]
    vnmsub.vx v8, a0, v3                 # v8 = -(v8 * 3) + v3  => v8 = [-35, -54, -97, -112]

# -- 11.14 widening multiply-add: 64-bit accumulator v12-v13
    vwaddu.vx v12, v1, x0                # accumulator = v1 widened = [10, 20, 30, 40] (e64)  => v12 = [10, 20, 30, 40] (64-bit)
    vwmaccu.vv  v12, v2, v3              # += unsigned v2 x unsigned v3  => v12 = [4294967301, 32, 12884901897, 72] (64-bit)
    vwaddu.vx v12, v1, x0   # => v12 = [10, 20, 30, 40] (64-bit)
    vwmaccu.vx  v12, a1, v2              # += unsigned 0xFFFFFFFE x unsigned v2  => v12 = [4294967304, 8589934608, 12884901912, 17179869216] (64-bit)
    vwaddu.vx v12, v1, x0   # => v12 = [10, 20, 30, 40] (64-bit)
    vwmacc.vv   v12, v2, v3              # += signed v2 x signed v3  => v12 = [5, 32, 9, 72] (64-bit)
    vwaddu.vx v12, v1, x0   # => v12 = [10, 20, 30, 40] (64-bit)
    vwmacc.vx   v12, a1, v2              # += -2 x v2  => v12 = [8, 16, 24, 32] (64-bit)
    vwaddu.vx v12, v1, x0   # => v12 = [10, 20, 30, 40] (64-bit)
    vwmaccsu.vv v12, v3, v2              # += signed v3 x unsigned v2  => v12 = [5, 32, 9, 72] (64-bit)
    vwaddu.vx v12, v1, x0   # => v12 = [10, 20, 30, 40] (64-bit)
    vwmaccsu.vx v12, a1, v2              # += signed -2 x unsigned v2  => v12 = [8, 16, 24, 32] (64-bit)
    vwaddu.vx v12, v1, x0   # => v12 = [10, 20, 30, 40] (64-bit)
    vwmaccus.vx v12, a1, v3              # += unsigned 0xFFFFFFFE x signed v3  => v12 = [-21474836460, 25769803784, -30064771028, 34359738392] (64-bit)

# -- 11.15 merge: vd[i] = v0.mask[i] ? second operand : vs2[i]   (v0 = [1 0 1 1])
    vmerge.vvm v8, v1, v2, v0            # take v2 where the mask is 1, else v1  => v8 = [1, 20, 3, 4]
    vmerge.vxm v8, v1, a1, v0            # take -2 where the mask is 1  => v8 = [-2, 20, -2, -2]
    vmerge.vim v8, v1, 7, v0             # take 7 where the mask is 1  => v8 = [7, 20, 7, 7]

# -- 11.16 move / splat
    vmv.v.v   v8, v3                     # copy a register  => v8 = [-5, 6, -7, 8]
    vmv.v.x   v8, a1                     # splat a scalar register  => v8 = [-2, -2, -2, -2]
    vmv.v.i   v8, 7                      # splat an immediate  => v8 = [7, 7, 7, 7]

# =================================================================================================
# 14  REDUCTIONS: vd[0] = vs1[0] (op) all vs2 elements; only element 0 of vd is written
# =================================================================================================
    vredsum.vs  v8, v1, v2               # v2[0] + 10 + 20 + 30 + 40  => v8[0] = 101
    vredand.vs  v8, v1, v3               # v3[0] & 10 & 20 & 30 & 40  => v8[0] = 0
    vredor.vs   v8, v1, v2               # v2[0] | 10 | 20 | 30 | 40  => v8[0] = 63
    vredxor.vs  v8, v1, v2               # v2[0] ^ 10 ^ 20 ^ 30 ^ 40  => v8[0] = 41
    vredminu.vs v8, v3, v2               # unsigned min of v2[0] and all of v3  => v8[0] = 1
    vredmin.vs  v8, v3, v2               # signed min  => v8[0] = -7
    vredmaxu.vs v8, v3, v2               # unsigned max  => v8[0] = -5
    vredmax.vs  v8, v3, v2               # signed max  => v8[0] = 8
    vwredsumu.vs v8, v3, v10             # 64-bit sum: v10[0] + zero-extended v3  => v8[0] = 12884901895 (64-bit)
    vwredsum.vs  v8, v3, v10             # 64-bit sum: v10[0] + sign-extended v3  => v8[0] = 4294967303 (64-bit)
    vredsum.vs  v8, v1, v2, v0.t         # masked: only elements where v0 = 1 (0, 2, 3)  => v8[0] = 81

# =================================================================================================
# 15  MASK INSTRUCTIONS (e8, vl = 8). Mask registers hold one bit per element.
#     v20 = mask [0 0 1 0 1 1 0 1]  (byte 180)     v21 = mask [0 1 1 0 0 1 1 0]  (byte 102)
# =================================================================================================
    vsetivli  x0, 8, e8, m1, ta, ma   # => vl = 8, vtype = e8, m1, ta, ma
    la        t3, masks
    vlm.v     v20, (t3)                  # v20  => v20 mask [0 0 1 0 1 1 0 1]
    addi      t3, t3, 1
    vlm.v     v21, (t3)                  # v21  => v21 mask [0 1 1 0 0 1 1 0]
# -- 15.1 mask logical (vs2 op vs1, bit by bit; never masked)
    vmand.mm  v8, v20, v21               # AND  => v8 mask [0 0 1 0 0 1 0 0]
    vmnand.mm v8, v20, v21               # NOT (AND)  => v8 mask [1 1 0 1 1 0 1 1]
    vmandn.mm v8, v20, v21               # v20 AND NOT v21  => v8 mask [0 0 0 0 1 0 0 1]
    vmxor.mm  v8, v20, v21               # XOR  => v8 mask [0 1 0 0 1 0 1 1]
    vmor.mm   v8, v20, v21               # OR  => v8 mask [0 1 1 0 1 1 1 1]
    vmnor.mm  v8, v20, v21               # NOT (OR)  => v8 mask [1 0 0 1 0 0 0 0]
    vmorn.mm  v8, v20, v21               # v20 OR NOT v21  => v8 mask [1 0 1 1 1 1 0 1]
    vmxnor.mm v8, v20, v21               # NOT (XOR)  => v8 mask [1 0 1 1 0 1 0 0]
#    pseudoinstructions built from the logical ops:
    vmmv.m    v8, v20                    # copy a mask      (vmand.mm v8, v20, v20)  => v8 mask [0 0 1 0 1 1 0 1]
    vmnot.m   v8, v20                    # invert a mask    (vmnand.mm v8, v20, v20)  => v8 mask [1 1 0 1 0 0 1 0]
    vmclr.m   v8                         # all zeros        (vmxor.mm v8, v8, v8)  => v8 mask [0 0 0 0 0 0 0 0]
    vmset.m   v8                         # all ones         (vmxnor.mm v8, v8, v8)  => v8 mask [1 1 1 1 1 1 1 1]
# -- 15.2 / 15.3 count and find: the result goes to an x register
    vcpop.m   a3, v20                    # number of set bits  => a3 = 4
    vfirst.m  a4, v20                    # index of the first set bit (-1 if none)  => a4 = 2
    vcpop.m   a5, v20, v0.t              # only bits where v0 = 1 (v0 = [1 0 1 1 0 0 0 0])  => a5 = 1
    vfirst.m  a6, v21, v0.t              # first set bit of v21 among the v0 elements  => a6 = 2
# -- 15.4 - 15.6 set before / including / only the first set bit
    vmsbf.m   v8, v20                    # 1s before the first 1  => v8 mask [1 1 0 0 0 0 0 0]
    vmsif.m   v8, v20                    # 1s up to and including the first 1  => v8 mask [1 1 1 0 0 0 0 0]
    vmsof.m   v8, v20                    # only the first 1  => v8 mask [0 0 1 0 0 0 0 0]
# -- 15.8 / 15.9 iota and id: SEW-wide results (here e8)
    viota.m   v8, v20                    # v8[i] = number of 1s in v20 before element i  => v8 = [0, 0, 0, 1, 1, 2, 3, 3]
    vid.v     v8                         # v8[i] = i  => v8 = [0, 1, 2, 3, 4, 5, 6, 7]
    vmv.v.i   v9, -1   # => v9 = [-1, -1, -1, -1, -1, -1, -1, -1]
    vid.v     v9, v0.t                   # masked: only elements 0, 2, 3; the rest keep -1  => v9 = [0, -1, 2, 3, -1, -1, -1, -1]

# =================================================================================================
# 16  PERMUTATION (e32, vl = 4)
# =================================================================================================
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma
# -- 16.1 scalar <-> element 0
    vmv.x.s   a3, v3                     # x[a3] = v3[0] (sign-extended)  => a3 = -5
    vmv.v.i   v8, 0   # => v8 = [0, 0, 0, 0]
    vmv.s.x   v8, a1                     # v8[0] = -2, the other elements unchanged  => v8 = [-2, 0, 0, 0]
# -- 16.3 slides
    vmv.v.i   v8, 0   # => v8 = [0, 0, 0, 0]
    li        t1, 1
    vslideup.vx   v8, v1, t1             # v8[i + 1] = v1[i]; v8[0] unchanged  => v8 = [0, 10, 20, 30]
    vmv.v.i   v8, 0   # => v8 = [0, 0, 0, 0]
    vslideup.vi   v8, v1, 2              # v8[i + 2] = v1[i]; v8[0..1] unchanged  => v8 = [0, 0, 10, 20]
    vslidedown.vx v8, v1, t1             # v8[i] = v1[i + 1]; past the end = 0  => v8 = [20, 30, 40, 0]
    vslidedown.vi v8, v1, 2              # v8[i] = v1[i + 2]  => v8 = [30, 40, 0, 0]
    vslide1up.vx  v8, v1, a1             # [-2, v1[0], v1[1], v1[2]]  => v8 = [-2, 10, 20, 30]
    vslide1down.vx v8, v1, a1            # [v1[1], v1[2], v1[3], -2]  => v8 = [20, 30, 40, -2]
# -- 16.4 gather: vd[i] = vs2[index[i]] (0 if the index is >= VLMAX)
    vid.v     v9                         # [0, 1, 2, 3]  => v9 = [0, 1, 2, 3]
    vrsub.vi  v9, v9, 3                  # indices [3, 2, 1, 0]  => v9 = [3, 2, 1, 0]
    vrgather.vv  v8, v1, v9              # reverse v1  => v8 = [40, 30, 20, 10]
    vrgather.vx  v8, v1, a0              # every element = v1[3]  => v8 = [40, 40, 40, 40]
    vrgather.vi  v8, v1, 5               # index 5 >= VLMAX: all 0  => v8 = [0, 0, 0, 0]
    vsetivli  x0, 4, e16, m1, ta, ma   # => vl = 4, vtype = e16, m1, ta, ma
    vid.v     v9   # => v9 = [0, 1, 2, 3]
    vrsub.vi  v9, v9, 3                  # e16 indices [3, 2, 1, 0]  => v9 = [3, 2, 1, 0]
    vsetivli  x0, 4, e32, m1, ta, ma   # => vl = 4, vtype = e32, m1, ta, ma
    vrgatherei16.vv v8, v1, v9           # same as vrgather.vv, but 16-bit indices  => v8 = [40, 30, 20, 10]
# -- 16.5 compress: pack the elements whose mask bit is 1 to the front
    vmv.v.i   v8, 0   # => v8 = [0, 0, 0, 0]
    vcompress.vm v8, v1, v0              # v0 = [1 0 1 1]: keep v1[0], v1[2], v1[3]  => v8 = [10, 30, 40, 0]
# -- 16.6 whole-register moves (ignore vl and vtype)
    vmv1r.v   v8, v1                     # copy 1 register  => v8 = [10, 20, 30, 40]
    vmv2r.v   v8, v10                    # copy 2 registers  (v10-v11 -> v8-v9)  => v8 = [5, 1, -64, -1]; v9 = [-16, 7, 1024, 0]
    vmv4r.v   v12, v8                    # copy 4 registers  (v8-v11 -> v12-v15)  => v12 = [5, 1, -64, -1]; v13 = [-16, 7, 1024, 0]; v14 = [5, 1, -64, -1]; v15 = [-16, 7, 1024, 0]
    vmv8r.v   v16, v8                    # copy 8 registers  (v8-v15 -> v16-v23)  => v16 = [5, 1, -64, -1]; v17 = [-16, 7, 1024, 0]; v18 = [5, 1, -64, -1]; v19 = [-16, 7, 1024, 0]; v20 = [5, 1, -64, -1]; v21 = [-16, 7, 1024, 0]; v22 = [5, 1, -64, -1]; v23 = [-16, 7, 1024, 0]
