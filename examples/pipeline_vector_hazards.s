# Vector pipeline hazards on the 5-stage pipeline with hazard detection and forwarding (H_F).
#
# H_F is the only VM that pipelines vector instructions over IF ID EX MEM WB: the result is
# computed in EX, vector loads/stores access memory in MEM, and the vector register is written
# in WB. The other pipelines run each vector instruction whole inside EX.
#
# GUI: Processor -> "5 stage Processor w/ hazard detection w/ forwarding", open View -> Pipeline
#      Panel (Ctrl+`), then press Step. On the Processor tab, the wires a vector instruction uses
#      light up green.
#
#   Part  What happens                                    H_F
#   1     vsetvli then a vector load                      new vl/vtype forwarded, no stall
#   2     vector load used by the next instruction        vector load-use: 1 stall (a nop)
#   3     vector result used by the next instruction      forwarded EX/MEM -> EX, no stall
#   4     scalar load used by a .vx instruction           load-use on the GPR: 1 stall
#   5     store of a register written just before         store data is read in MEM, no stall
#   6     parts 2 and 4 again, reordered                  independent work in between, no stall
#
# Cycles on H_F: 27 = 21 instructions + 4 to fill the pipeline + 2 stalls.
# Expected: out (0x10000020) = 66 132 198 264, and v8 = 3 6 9 12 (element 0 first)
#           In the Vector tab, v3 = 11 22 33 44, v4 = 22 44 66 88, v5 = 66 132 198 264

.data
a:      .word 1, 2, 3, 4
b:      .word 10, 20, 30, 40
out:    .word 0, 0, 0, 0
scale:  .word 3

.text
    la      s0, a
    la      s1, b
    la      s2, out
    la      s3, scale
    li      a0, 4

    # 1  vl and vtype from the vsetvli reach the load without waiting
    vsetvli t0, a0, e32, m1, ta, ma   # vl = 4
    vle32.v v1, (s0)                  # v1 = 1 2 3 4
    vle32.v v2, (s1)                  # v2 = 10 20 30 40

    # 2  vector load-use: v2 is only loaded at the end of MEM
    vadd.vv v3, v1, v2                # v3 = 11 22 33 44   (1 stall)

    # 3  vector forwarding: v3 comes from EX/MEM
    vadd.vv v4, v3, v3                # v4 = 22 44 66 88   (no stall)

    # 4  scalar load feeding a vector instruction
    lw      t1, 0(s3)                 # t1 = 3
    vmul.vx v5, v4, t1                # v5 = 66 132 198 264 (1 stall)

    # 5  the store reads v5 in MEM, after vmul has written it back
    vse32.v v5, (s2)                  # out = 66 132 198 264 (no stall)

    # 6  the same kind of work, with independent instructions placed between producer and user
    lw      t2, 0(s3)                 # t2 = 3
    vle32.v v6, (s0)                  # v6 = 1 2 3 4
    vadd.vv v7, v1, v1                # independent: fills the slot
    vmul.vx v8, v6, t2                # v8 = 3 6 9 12      (no stall)
