# ============================================================================================
#  Dot product -- VECTOR version (RVV 1.0)
# ============================================================================================
#  C equivalent:
#      int32_t a[256], b[256];
#      int32_t sum = 0;
#      for (int i = 0; i < 256; i++)
#          sum += a[i] * b[i];
#      out = sum;
#
#  Strip-mining: each pass, vsetvli sets vl = min(elements left, VLMAX). With 32-bit
#  elements (e32), register groups of 8 (m8) and VLEN = 128 bits, VLMAX = 8 * 128 / 32 = 32,
#  so the 256 elements take 8 passes. The same code runs unchanged on a machine with a
#  longer VLEN; it just needs fewer passes.
#  Each pass multiplies vl pairs at once (vmul.vv) and adds all vl products into
#  element 0 of v24 with one reduction (vredsum.vs). v24[0] carries the running sum.
#
#  Registers:
#      a0  &a[i]          a1  &b[i]          a2  &out           a3  elements left
#      a4  the final sum  t0  vl             t1  vl * 4 (bytes this pass)
#      v0-v7  a[i..]      v8-v15  b[i..]     v16-v23  products   v24[0]  running sum
#
#  Instructions executed (dynamic count):
#      setup  9   (3 x la = 6, li, vsetvli, vmv.s.x)
#      loop   10 instructions x 8 passes = 80
#      end    2   (vmv.x.s, sw)
#      total  91
#
#  Result in `out` (0x10000000):
#      out = -43609
#
#  Run:  kites-cli benchmarks/programs/dot_vector.s --stats --mem-dump 0x10000000 1
# ============================================================================================

        .text
        la      a0, a                   # a0 = &a[0]
        la      a1, b                   # a1 = &b[0]
        la      a2, out                 # a2 = &out
        li      a3, 256                 # a3 = number of elements
        vsetvli t0, x0, e32, m1, ta, ma # any vl > 0, so that vmv.s.x writes element 0
        vmv.s.x v24, zero               # v24[0] = sum = 0
loop:
        vsetvli t0, a3, e32, m8, ta, ma # t0 = vl = min(a3, VLMAX)
        vle32.v v0, (a0)                # v0-v7   = a[i .. i+vl-1]
        vle32.v v8, (a1)                # v8-v15  = b[i .. i+vl-1]
        vmul.vv v16, v0, v8             # v16-v23 = a[i] * b[i] for vl elements
        vredsum.vs v24, v16, v24        # v24[0] = v24[0] + sum of the vl products
        slli    t1, t0, 2               # t1 = bytes processed = vl * 4
        add     a0, a0, t1              # advance both pointers by vl elements
        add     a1, a1, t1
        sub     a3, a3, t0              # vl elements done
        bnez    a3, loop                # repeat while elements are left
        vmv.x.s a4, v24                 # a4 = v24[0] = sum
        sw      a4, 0(a2)               # out = sum

        .data
out:    .zero   4                       # the dot product (written by the program)

# Inputs: 256 random int32 values each, in [-100, 100].
a:      .word    -40,   51,   39,  -67,   -6,   54,   21,   60,   48,  -84,   55,  -97,   20,  -34,   41,  -41
        .word    -51,   83,   20,   38,   40,   21,    1,   63,  -62,  -41,   62,  -62,   33,   -1,   89,  -97
        .word     71,   98,  -84,  -60,   94,   51,  -90,  -23,   99,  -93,  -32,   21,   52,   84,   -1,   82
        .word      9,    1,   86,   47,   13,  -66,   -7,  -76,  -91,  -66,   26,  -45,  -34,   72,   11,   99
        .word     60,  -23,    7,   29,   -2,   46,  -11,   36,   49,    4,   49,  -41,  -14,   74,  -93,  -29
        .word     55,   71,   78,  -59,   78,  -17,   38,   46,   45,  -74,   82,   67,  -46,   62,   46,  -32
        .word    -28,  -69,  -84,   23,   63,   23,  -78,  -12,  -83,    5,  -62,  -95,  -25,    9,   96,    6
        .word    -70,  -89,   54,   57,   94,  -89,   -4,   83,   50,  -16,   41,  -29,   29,  -40,  -91,  -21
        .word    -99,  -81,  -73,   53,   37,  -92,  -50,    4,  -26,   56,  -33,  -61,   76,  -90,  -14,  -20
        .word     -8,  -65,   -4,   -4,   17,   33,   -2,   64,   52,   74,   43,  -74,   58,   29,  -31,   10
        .word     62,   84,   83,  -40,  -23,   11,  -34,   33,  -23,   40,  -14,  -98,    6,   48,  -20,  -95
        .word     -4,   57,   50,   61,  -66,  -85,   62,   60,  -15,   19,  -10,   73,  -10,   55,   80,  -29
        .word     88,   25,  -95,   50,  -85,   73,  -95,   -6,  -36,   60,   16,  -24,   51,   53,  -19,  -55
        .word     -7,  -53,  -20,   94,   -6,   52,  -33,  -24,   -4,  -74,   97,  -94,   45,   75,   88,  -67
        .word    -21,   28,  -44,   67,  -32,  -39,  -17,  -53,   73,   11,   66,   78,  -76,  -74,   53,  -18
        .word    -15,   72,  -43,   12,  -57,  -80,  -14,   89,   66,  -45,   45,   15,  -31,  -43,  -70,  -92

b:      .word     35,  -52,  -20,   47,  -54,  -29,  -13,   64,  -79,   58,  -12,   50,  -67,    7,  -26,   32
        .word    -31,   18,  -12,   62,    6,  -26,    7,   45,    4,  -91,    5,  -61,  -49,  -99,   22,   59
        .word     30,   11,   43,   83,  -44,  -92,   90,   16,   92,   69,   91,   32,  -27,   39,  -13,  -42
        .word    -83,   50,  -27,  -70,  -38,  -89,  -92,   77,   31,  -50,   10,   47,  -88,  -97,   23,   90
        .word    -70,  -57,   28,  -24,  -39,   69,  -95,   34,   37,    5,  -87,   56,  -71,  -13,  -68,  -36
        .word     38,   22,  100,  -85,  -10,  -44,  -50,  -69,   36,  -70,  -57,  -39,  -30,  -68,  -99,   24
        .word     60,   46,    2,  -88,   93,  -31,  -37,  -32,   58,   34,   33,    8,  -87,   21,  -18,   98
        .word   -100,  -86,   98,  -68,  -89,  -69,  -88,  -83,   23,  -92,   82,  -78,   31,   28,   25,  -20
        .word    -60,  -20,  -82,  -11,   -2,   65,   -1,   50,  -23,   -8,  -33,  -52,  -16,    9,  -69,  -68
        .word     42, -100,   83,   85,   -3,  -80,   45,  -55,  -90,   -5,   17,   54,   66,  100,   38,   -3
        .word     62,  -89,   59,   10,  -87,   -5,   60,   27,   94,   79,  -20,    7,   77,    7,   17,  -96
        .word    -38,  -45,   37,  -31,   77,   51,  -82,    8,  -43,    9,  -67,  -93,  -17,   -5,   43,  -33
        .word    -69,   18,   76,  -69,   87,   69,   35,   -4,   70,  -73,   87,  -19,   44,   36,  -74,   50
        .word     83,  -99,   21,  -64,  -40,   98,   -1,  -89,   34,  -77,   44,  -75,   68,   -4,  -55,  -94
        .word    -13,  -69,  -94,  -71,   72,   23,   78,  -28,   48,  -24,  -78,  -91,   96,   44,   30,   35
        .word     83,  -39,  -73,   41,   91,  -75,   41,  -85,   40,  -17,   44,  -54,  -81,  -39,  -54,   65
