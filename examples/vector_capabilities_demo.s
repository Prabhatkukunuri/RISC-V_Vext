# =============================================================================
# Kites vector (RVV) capabilities demo
#
# Run:   ./build/kites-cli examples/vector_capabilities_demo.s
# Check: ~/Library/Application Support/Kites/vm_state/registers_dump.json
#        "gp_registers"   -> results of vset* and CSR reads
#        "vec_registers"  -> one 128-bit hex value per register (VLEN = 128),
#                            printed MOST-significant byte first, so element 0
#                            is the RIGHT-most group of digits.
#
# Uses the default single-cycle VM (rvss): it also runs the csrrs reads.
# =============================================================================

.text
# -----------------------------------------------------------------------------
# 1. Configuration: vsetvli / vsetivli / vsetvl            (results in x8-x13)
# -----------------------------------------------------------------------------
    vsetvli  s0, x0, e32, m1, ta, ma    # rs1 = x0, rd != x0: vl = VLMAX = 128/32 = 4  -> s0 (x8)  = 4
    li       a0, 3
    vsetvli  s1, a0, e8, m1, ta, ma     # AVL 3 <= VLMAX 16: vl = 3                    -> s1 (x9)  = 3
    vsetivli a7, 31, e16, m2, ta, ma    # AVL 31 > VLMAX 2*128/16 = 16: vl = 16        -> a7 (x17) = 16
    li       a1, 0xd0                   # vtype for e32, m1, ta, ma
    li       a2, 100
    vsetvl   s2, a2, a1                 # vtype from a register; AVL 100 -> VLMAX 4   -> s2 (x18) = 4
    csrrs    s3, vtype, x0              # read vtype                                  -> s3 (x19) = 0xd0
    csrrs    s4, vlenb, x0              # read vlenb = VLEN/8                         -> s4 (x20) = 16
    csrrs    s5, vl, x0                 # read vl                                     -> s5 (x21) = 4

# -----------------------------------------------------------------------------
# 2. Build test data without vector loads, using tail-undisturbed behaviour:
#    each vadd.vi only touches elements 0..vl-1 and leaves the rest alone.
# -----------------------------------------------------------------------------
    vsetivli x0, 1, e8, m1, ta, ma
    vadd.vi  v0, v0, 5                  # mask register v0: byte 0 = 0b0101 (elements 0 and 2 active)
                                        # v0  = 0x00000000000000000000000000000005

    vsetivli x0, 4, e32, m1, ta, ma
    vadd.vi  v1, v1, 1                  # v1 = [1, 1, 1, 1]
    vsetivli x0, 3, e32, m1, ta, ma
    vadd.vi  v1, v1, 1                  # v1 = [2, 2, 2, 1]
    vsetivli x0, 2, e32, m1, ta, ma
    vadd.vi  v1, v1, 1                  # v1 = [3, 3, 2, 1]
    vsetivli x0, 1, e32, m1, ta, ma
    vadd.vi  v1, v1, 1                  # v1 = [4, 3, 2, 1]   (element 0 first)
                                        # v1  = 0x00000001000000020000000300000004

# -----------------------------------------------------------------------------
# 3. Single-width integer add / subtract  (e32, vl = 4, t0 = 10)
# -----------------------------------------------------------------------------
    vsetivli x0, 4, e32, m1, ta, ma
    li       t0, 10
    vadd.vv  v2, v1, v1                 # [8, 6, 4, 2]          v2  = 0x00000002000000040000000600000008
    vadd.vx  v3, v1, t0                 # [14, 13, 12, 11]      v3  = 0x0000000b0000000c0000000d0000000e
    vadd.vi  v4, v1, -5                 # [-1, -2, -3, -4]      v4  = 0xfffffffcfffffffdfffffffeffffffff
    vsub.vv  v5, v3, v1                 # [10, 10, 10, 10]      v5  = 0x0000000a0000000a0000000a0000000a
    vsub.vx  v6, v1, t0                 # [-6, -7, -8, -9]      v6  = 0xfffffff7fffffff8fffffff9fffffffa
    vrsub.vx v7, v1, t0                 # 10 - v1 = [6,7,8,9]   v7  = 0x00000009000000080000000700000006
    vrsub.vi v8, v1, 0                  # 0 - v1 (negate)       v8  = 0xfffffffffffffffefffffffdfffffffc

# Masked: only elements whose bit in v0 is 1 (elements 0 and 2) are written;
# elements 1 and 3 keep their old value (0).
    vadd.vi  v9, v1, 10, v0.t           # [14, 0, 12, 0]        v9  = 0x000000000000000c000000000000000e

# -----------------------------------------------------------------------------
# 4. Bitwise logical  (e32, vl = 4)
# -----------------------------------------------------------------------------
    vand.vv  v10, v3, v1                # [14&4, 13&3, 12&2, 11&1] = [4, 1, 0, 1]
                                        #                       v10 = 0x00000001000000000000000100000004
    vor.vi   v11, v1, 8                 # [12, 11, 10, 9]       v11 = 0x000000090000000a0000000b0000000c
    vxor.vi  v12, v1, -1                # NOT v1                v12 = 0xfffffffefffffffdfffffffcfffffffb
    vxor.vx  v13, v1, t0                # v1 ^ 10 = [14,9,8,11] v13 = 0x0000000b00000008000000090000000e

# -----------------------------------------------------------------------------
# 5. Widening add / subtract  (e32 sources -> e64 results in 2-register groups)
# -----------------------------------------------------------------------------
    vwaddu.vv v14, v1, v3               # [18, 16, 14, 12] as e64 in v14 (elements 0-1) and v15 (2-3)
                                        #                       v14 = 0x00000000000000100000000000000012
                                        #                       v15 = 0x000000000000000c000000000000000e
    vwsub.vx  v16, v1, t0               # signed v1 - 10 = [-6, -7, -8, -9] as e64
                                        #                       v16 = 0xfffffffffffffff9fffffffffffffffa
                                        #                       v17 = 0xfffffffffffffff7fffffffffffffff8
    vwadd.wv  v18, v14, v1              # wide v14 + v1 = [22, 19, 16, 13] as e64
                                        #                       v18 = 0x00000000000000130000000000000016
                                        #                       v19 = 0x000000000000000d0000000000000010

# -----------------------------------------------------------------------------
# 6. Integer extension
# -----------------------------------------------------------------------------
    vsetivli x0, 4, e64, m2, ta, ma     # e64 results, 2-register groups (VLMAX = 4)
    vsext.vf2 v20, v4                   # sign-extend e32 [-1,-2,-3,-4] to e64
                                        #                       v20 = 0xfffffffffffffffeffffffffffffffff
                                        #                       v21 = 0xfffffffffffffffcfffffffffffffffd
    vzext.vf2 v22, v4                   # zero-extend the same values
                                        #                       v22 = 0x00000000fffffffe00000000ffffffff
                                        #                       v23 = 0x00000000fffffffc00000000fffffffd

    vsetivli x0, 4, e8, m1, ta, ma
    vadd.vi  v24, v24, -3               # 4 bytes of 0xFD       v24 = 0x000000000000000000000000fdfdfdfd
    vsetivli x0, 4, e32, m1, ta, ma
    vsext.vf4 v25, v24                  # e8 -> e32, signed: -3 v25 = 0xfffffffdfffffffdfffffffdfffffffd
    vzext.vf4 v26, v24                  # e8 -> e32, unsigned   v26 = 0x000000fd000000fd000000fd000000fd

# -----------------------------------------------------------------------------
# 7. LMUL = 2: one instruction writes a 2-register group (v28 and v29)
# -----------------------------------------------------------------------------
    vsetvli  s6, x0, e32, m2, ta, ma    # VLMAX = 2*128/32 = 8                       -> s6 (x22) = 8
    vadd.vi  v28, v28, 7                # 8 elements of 7       v28 = 0x00000007000000070000000700000007
                                        #                       v29 = 0x00000007000000070000000700000007

# -----------------------------------------------------------------------------
# Final vector CSRs ("control and status registers" in the dump):
#   vl = 8, vtype = 0xd1 (e32, m2, ta, ma), vstart = 0, vlenb = 0x10 (16)
# All other vector registers stay 0.
# -----------------------------------------------------------------------------
