# Examples

The `examples/` folder holds small assembly programs you can open in the Kites GUI or run with
`kites-cli`. This guide walks through the ones that show two features best:

- [the 5-stage pipeline](#watching-the-pipeline): stalls, forwarding and flushes, for scalar and
  vector instructions;
- [the Bicameral Cache](#the-bicameral-cache): a split L1 with a Scalar Cache and a Vector Cache.

## Running an example

**In the GUI**

1. **File → Open** the `.s` file. It opens in the Editor tab.
2. Press **Processor** in the toolbar and choose the VM the example asks for.
3. Press **Step** to advance one clock cycle, or **Run** to run to the end. **Undo** steps back.
4. Look at the result in the Processor, Memory, Vector or Cache tab, as the example says.

**On the command line** (build `kites-cli` first; see the README):

```bash
./build/kites-cli examples/<file>.s --vm rv5-h-f --stats
```

On macOS the register and memory dumps go to `~/Library/Application Support/Kites/vm_state/`;
elsewhere to `./vm_state/`. See [CLI.md](CLI.md) for every option.

## Watching the pipeline

Choose **5 stage Processor w/ hazard detection w/ forwarding** and open **View → Pipeline Panel**
(`` Ctrl+` ``). The panel at the bottom of the window draws a space-time diagram: one row per
instruction, one column per clock cycle, and in each cell the stage the instruction is in
(IF, ID, EX, MEM, WB). A stall bubble or a flushed instruction is a row named `nop`. Drag the
panel's top edge to make it taller; it follows the colours of the theme you pick in Preferences.

On the **Processor** tab, the datapath highlights the wires each stage uses in the current cycle.
Wires carrying a vector instruction are drawn in green.

### `pipeline_scalar_hazards.s`: stalls, forwarding and a flush

Step through it and you see, in order:

| Part | Hazard | What the panel shows |
|---|---|---|
| 1 | `add` uses the result of the `addi` right before it | no `nop`: the value is forwarded from EX/MEM |
| 2 | `add` uses a result from two instructions back | no `nop`: forwarded from MEM/WB |
| 3 | `add` uses the value a `lw` just loaded | one `nop`: the load-use stall |
| 4 | `beq` is taken | three `nop` rows: the instructions fetched behind the branch are flushed |

```asm
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
```

### `pipeline_vector_hazards.s`: vector instructions in the pipeline

Only the hazard-detection-and-forwarding pipeline (H_F) runs vector instructions through all five
stages: the result is computed in EX, vector loads and stores access memory in MEM, and the
vector register is written in WB. It has a vector forwarding path (EX/MEM → EX) and its own
vector hazard detection unit. The other three pipelines run each vector instruction whole in EX.

| Part | Hazard | What the panel shows |
|---|---|---|
| 1 | `vle32.v` right after `vsetvli` | no `nop`: the new `vl`/`vtype` is forwarded |
| 2 | `vadd.vv` uses `v2`, loaded by the previous instruction | one `nop`: the vector load-use stall |
| 3 | `vadd.vv` uses `v3`, computed by the previous instruction | no `nop`: vector forwarding |
| 4 | `vmul.vx` uses `t1`, loaded by the previous `lw` | one `nop`: scalar load-use |
| 5 | `vse32.v` stores `v5`, computed just before | no `nop`: store data is read in MEM |
| 6 | the same work as parts 2 and 4, reordered | no `nop`: independent work fills the gap |

Open the **Vector** tab to see `v1` to `v8` change, each in the cycle its instruction reaches WB.

```asm
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
```

### Comparing the four pipelines

Run either example on each 5-stage VM (toolbar **Processor**, or `--vm` on the command line). The
two without hazard detection run faster but compute wrong values, because nothing stops an
instruction from reading a register before it has been written:

| VM | `pipeline_scalar_hazards.s` | `pipeline_vector_hazards.s` |
|---|---|---|
| `rv5-nh-nf` (no detection, no forwarding) | 20 cycles, **wrong** (`t1`, `t3`, `t6` and `nums[2]`) | 25 cycles, **wrong** (`out` is left 0) |
| `rv5-h-nf` (detection, no forwarding) | 31 cycles, correct: stalls until each value is written back | 37 cycles, correct |
| `rv5-nh-f` (forwarding, no detection) | 20 cycles, **wrong**: the load-use in part 3 is missed | 25 cycles, **wrong** `out` |
| `rv5-h-f` (detection and forwarding) | 21 cycles, correct | 27 cycles, correct |
| `rvss` (single cycle, for reference) | 13 cycles | 21 cycles |

The other `*_hazard.s` files in `examples/` (and `examples/fd-extension/` for floating point) each
isolate one more pair of instructions, for example `L_U_hazard.s` (load then use) or
`R_B_hazard.s` (ALU result then branch).

## The Bicameral Cache

The Bicameral Cache replaces the L1 data cache with two halves of equal size:

| Half | Organisation | Used by |
|---|---|---|
| Scalar Cache (SC) | set associative, block = one sector | scalar loads and stores (`lw`, `sw`, ...) |
| Vector Cache (VC) | fully associative; each line is several sectors | vector loads and stores (`vle*`, `vse*`) |

- A VC miss is a **sector miss** when the line is there but the sector isn't (fetch one sector,
  evict nothing), or a **line miss** when no line holds the block (evict a whole line, writing
  back only its dirty sectors).
- A vector access whose sector is in the SC **migrates** it to the VC. A scalar access whose sector
  is in the VC is a **cross-hit**: it is served there and stays there. A sector is never in both.

**In the GUI**, open the **Cache** tab, then the **Bicameral L1** page: tick **Enable**, set the
sizes the example lists, and press **Apply**. The page shows the Scalar Cache table, the Vector
Cache lines with one character per sector (`V` valid, `D` dirty, `-` empty), and running
statistics. **Write back to memory** flushes dirty data so the Memory tab shows it.

**On the command line**, `--bicameral` uses a fixed 8 KB L1 (SC 16 indices × 4 ways × 64 B,
VC 4 lines × 16 sectors × 64 B) and `--stats` prints its counters.

### `bicameral_cache_check.s`: one event per step

Each step causes one kind of cache event: a line miss, a sector miss, a dirty sector, a scalar
miss and hit, a migration, a cross-hit, an LRU update and a line eviction with write-back. Set the
small geometry in the file's header (1 KB L1, 4 sectors per VC line) so evictions happen after a
few accesses, then Step and compare the Bicameral L1 page with the comment above each instruction.

```asm
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
```

The numbers in the comments are for the 1 KB GUI geometry. On the command line's 8 KB geometry
(`kites-cli examples/bicameral_cache_check.s --bicameral --stats`) block 8 lands on a free line,
so the result is slightly different:

```
Scalar Cache : 1 hits, 1 misses, 0 write-backs
Vector Cache : 19 hits, 2 sector misses, 3 line misses, 2 sector write-backs
Cross lookup : 1 SC->VC migrations, 1 scalar cross-hits in the VC
```

### `bicameral_fir_bank.s`: why the split helps

A 1024-tap FIR filter runs over 8 audio channels. The taps are read with scalar loads and reused
by every channel; each channel's samples are read with vector loads once. In a normal 8 KB L1,
each new channel's samples push the taps out, so they miss again for every channel. In the
Bicameral Cache, the samples only enter the Vector Cache, so the taps stay in the Scalar Cache and
miss only the first time. The program is 57,425 instructions, so use **Run**, not Step. Only the
code is quoted here; the file also holds the tap and sample data.

```asm
# ============================================================================================
#  Multi-channel FIR equaliser
# ============================================================================================
#  One 1024-tap FIR filter (a smooth low-pass window) is applied to 8 independent audio channels:
#      y[c][n] = sum_{k=0}^{1023} h[k] * x[c][n + k]      c = 0..7, n = 0..31
#
#  Why it suits the Bicameral Cache:
#    - the taps h[] (4 KB) are read by SCALAR loads (lw) and reused by every channel
#      -> temporal locality, exactly the Scalar Cache's size (16 indices x 4 ways x 64 B);
#    - each channel's input frame (1055 samples, ~4.1 KB) is read by VECTOR loads in one sweep
#      and never again -> a pure spatial stream, handled by the Vector Cache.
#    In a normal 8 KB L1 the taps and a frame (8.1 KB) do not fit together, so every new frame
#    pushes the taps out and they miss again for every channel. In the Bicameral Cache the
#    frames only ever enter the Vector Cache, so the taps stay in the Scalar Cache: they miss
#    64 times in total (once per block) instead of about 64 times per channel.
#
#  Settings (Cache tab), everything else default:
#    Normal run  -- L1 Cache tab: 8 KB, 64 B blocks (16 words), LRU, write-back + write-allocate
#                   e.g. 32 sets x 4 ways; L2: 64 indices x 4 ways x 256 B
#    Bicameral   -- Bicameral L1 page: Enable, L1 8 KB, sector 64 B, sectors per VC line 2
#                   (any value gives the same result here), Scalar Cache associativity 4,
#                   write-back; L2 as above
#
#  Measured L1 data misses (270,592 data accesses in all):
#    Normal L1, 8 KB, 64 B blocks:   direct-mapped 868 | 2-way 698 | 4-way 716 | 8-way 752
#                                    16-way 903 | 32-way 1061 | fully associative 1064
#    Bicameral, 8 KB, 64 B sectors:  616 = Scalar Cache 64 (taps, first touch only)
#                                        + Vector Cache 552 (the 8 frames and the outputs)
#
#  Run:  kites-cli examples/bicameral_fir_bank.s --bicameral --stats
# ============================================================================================

                .text
        lui     a0, 0x10000             # a0 = 0x10000000 = &h[0]
        lui     a1, 0x10001             # a1 = 0x10001000 = &x[0][0]
        lui     a2, 0x10009             # a2 = 0x10009000
        addi    a2, a2, 992             #    + 0x3E0 = 0x100093E0 = &y[0][0]
        addi    a3, zero, 8             # channels left
        addi    a4, zero, 1024          # taps
        lui     a5, 1                   # a5 = 4096
        addi    a5, a5, 124             #    + 124 = 4220 bytes per frame
        vsetvli t0, zero, e32, m8, ta, ma   # vl = VLMAX = 32
channel:
        vmv.v.i v8, 0                   # acc[0..31] = 0
        addi    t1, a0, 0               # t1 = &h[0]
        addi    t2, a1, 0               # t2 = &x[c][0]
        addi    t3, a4, 0               # t3 = taps left
tap:
        lw      t4, 0(t1)               # h[k]                 (scalar)
        vle32.v v16, (t2)               # x[c][k .. k+31]      (vector)
        vmacc.vx v8, t4, v16            # acc += h[k] * x[c][k ..]
        addi    t1, t1, 4
        addi    t2, t2, 4
        addi    t3, t3, -1
        bne     t3, zero, tap
        vse32.v v8, (a2)                # y[c][0..31] = acc
        add     a1, a1, a5              # next frame
        addi    a2, a2, 128             # next output row
        addi    a3, a3, -1
        bne     a3, zero, channel
end:
```

With `kites-cli examples/bicameral_fir_bank.s --bicameral --stats`:

```
Scalar Cache : 8128 hits, 64 misses, 0 write-backs
Vector Cache : 261848 hits, 510 sector misses, 42 line misses, 24 sector write-backs
Cross lookup : 0 SC->VC migrations, 0 scalar cross-hits in the VC
```

That is 616 L1 misses in all: 64 for the taps (each 64 B block once) and 552 for the samples
and outputs. The best normal 8 KB L1 the file's header lists, 2-way, has 698. To compare in the
GUI, run once with the normal **L1** cache configured as the header says and once with
**Bicameral L1** enabled.
