# Bicameral Cache check: one step per kind of event, so each can be seen in the GUI.
#
# Cache tab -> Bicameral L1, set:
#   Enable                       ticked
#   L1 size                      1 KB
#   Sector size = SC block size  64 B
#   Sectors per VC line          4        -> VC line = 256 B, Vector Cache lines = 2
#   Scalar Cache associativity   2        -> Scalar Cache indices = 4
#   Write policy                 Write-back
#   L2 indices                   8
#   L2 associativity             2
#   L2 block size                64 B     -> L2 size = 1 KB
# then press Apply.
#
# L2 tab (policies; these are the defaults, check they are still set):
#   Write hit    Write-through    (an evicted dirty sector goes straight on to memory,
#                                  which S9 relies on for the Memory tab check)
#   Write miss   No write-allocate
#   Replacement  LRU
#
# With that geometry (B = 0x10000000):
#   VC: block = addr / 256, sector = (addr / 64) % 4      (fully associative, 2 lines)
#   SC: index = (addr / 64) % 4, 2 ways, 64 B blocks
#
# A vle32/vse32 with vl = 4 is 4 element accesses of 4 B, all in one 64 B sector:
# the first one decides hit / sector miss / line miss, the other three are hits.

.data
first:  .word 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16   # B+0x000, sector 0
second: .word 17, 18, 19, 20                                          # B+0x040, sector 1

.text
    li      s0, 0x10000000          # B
    li      a0, 4
    vsetvli t0, a0, e32, m1, ta, ma # vl = 4 (VLEN 128)

    # S1  vector load, block 0 sector 0: LINE MISS (VC line 0 gets block 0)
    #     VC: hits 3, line misses 1          sector map V---      v1 = 1 2 3 4
    vle32.v v1, (s0)

    # S2  vector load, block 0 sector 1: SECTOR MISS (same line, nothing evicted)
    #     VC: hits 6, sector misses 1        sector map VV--      v2 = 17 18 19 20
    addi    t1, s0, 0x40
    vle32.v v2, (t1)

    # S3  vector store, block 0 sector 2: SECTOR MISS, then the sector is dirty
    #     VC: hits 9, sector misses 2        sector map VVD-      stores 18 20 22 24
    vadd.vv v3, v1, v2
    addi    t1, s0, 0x80
    vse32.v v3, (t1)

    # S4  scalar store to B+0x400 (block 4, SC index 0): SC MISS, line filled, dirty
    #     SC: misses 1
    li      t2, 42
    li      s1, 0x10000400
    sw      t2, 0(s1)

    # S5  scalar load, same address: SC HIT
    #     SC: hits 1                         t3 = 42
    lw      t3, 0(s1)

    # S6  vector load of B+0x400: LINE MISS in the VC, but the sector is in the SC,
    #     so it MIGRATES: the SC block is invalidated, VC line 1 gets it (still dirty)
    #     VC: hits 12, line misses 2; migrations 1
    #     SC table: that block is gone       VC line 1 map D---   v4 = 42 0 0 0
    vle32.v v4, (s1)

    # S7  scalar load of B+0x044 (block 0 sector 1, only in the VC): CROSS-HIT,
    #     served from the VC and NOT moved back to the SC
    #     cross-hits 1                       t4 = 18; SC table unchanged
    lw      t4, 0x44(s0)

    # S8  vector load of B+0x410 (block 4 sector 0 again): HIT, makes VC line 1 the
    #     most recently used, so line 0 is the LRU line
    #     VC: hits 16                        v5 = 0 0 0 0
    addi    t1, s1, 0x10
    vle32.v v5, (t1)

    # S9  vector load of B+0x800 (block 8): LINE MISS with both lines in use:
    #     LRU line 0 (block 0) is evicted. Only its dirty sector 2 is written back
    #     (sectors 0 and 1 are clean and just dropped)
    #     VC: hits 19, line misses 3, write-backs 1      VC line 0 map V--- (block 8)
    #     Memory tab: B+0x80 now shows 18 20 22 24
    #     B+0x400 still shows 0: the 42 is dirty in VC line 1. Press
    #     "Write back to memory" and it appears.
    li      s2, 0x10000800
    vle32.v v6, (s2)

# Final statistics:
#   Scalar Cache  hits 1, misses 1, WB 0
#   Vector Cache  hits 19, sector misses 2, line misses 3, WB 1
#   Cross lookup  migrations 1, cross-hits 1
