# Scalar pipeline hazards: one of each kind, to watch in the Pipeline panel.
#
# GUI: Processor -> "5 stage Processor w/ hazard detection w/ forwarding", open the panel with
#      View -> Pipeline Panel (Ctrl+`), then press Step and watch each instruction move through
#      IF ID EX MEM WB. A stall shows as a "nop" row; a taken branch turns the three wrongly
#      fetched instructions into "nop" rows.
#
#   Part  What happens                              H_F (hazard detection + forwarding)
#   1     ALU result used by the next instruction   forwarded EX/MEM -> EX, no stall
#   2     ALU result used two instructions later    forwarded MEM/WB -> EX, no stall
#   3     load result used by the next instruction  load-use: 1 stall (a nop), then MEM/WB -> EX
#   4     taken branch                              resolved in EX, PC redirected a cycle later:
#                                                   the 3 instructions fetched after it are
#                                                   flushed (3 nops)
#
# Cycles on H_F: 21 = 13 instructions + 4 to fill the pipeline + 1 stall + 3 flushed.
# Expected registers: t1 = 20, t3 = 13, t5 = 7, t6 = 14, s1 = 1 (the flushed addi never runs)
# Expected memory:    nums[2] (0x10000008) = 14

.data
nums:   .word 5, 7, 0

.text
    la    s0, nums

    # 1  EX/MEM -> EX forwarding
    addi  t0, zero, 10
    add   t1, t0, t0          # t1 = 20: t0 comes straight from the previous instruction

    # 2  MEM/WB -> EX forwarding
    addi  t2, zero, 3
    addi  zero, zero, 0       # an unrelated instruction in between
    add   t3, t2, t0          # t3 = 13: t2 comes from two instructions back

    # 3  load-use: the loaded value is only ready at the end of MEM
    lw    t5, 4(s0)           # t5 = 7
    add   t6, t5, t5          # t6 = 14: waits one cycle for t5
    sw    t6, 8(s0)           # nums[2] = 14

    # 4  taken branch
    addi  s1, zero, 1
    beq   s1, s1, done        # always taken
    addi  s1, zero, 99        # fetched, then flushed
    addi  s1, zero, 98        # fetched, then flushed
    addi  s1, zero, 97        # fetched, then flushed
done:
    addi  zero, zero, 0
