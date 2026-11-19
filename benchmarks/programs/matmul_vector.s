# ============================================================================================
#  Matrix multiplication 16x16 -- VECTOR version (RVV 1.0)
# ============================================================================================
#  C equivalent:
#      int32_t A[16][16], B[16][16], C[16][16];      // row-major
#      for (int i = 0; i < 16; i++)
#          for (int j = 0; j < 16; j++) {
#              int32_t sum = 0;
#              for (int k = 0; k < 16; k++)
#                  sum += A[i][k] * B[k][j];
#              C[i][j] = sum;
#          }
#
#  Vectorised over j: a whole row of C (16 elements) is computed at once.
#      C[i][0..15] = sum over k of  A[i][k] * B[k][0..15]
#  For each k, one scalar A[i][k] multiplies the whole row k of B and is accumulated with
#  vmacc.vx. With e32 and m4 (groups of 4 registers), VLMAX = 4 * 128 / 32 = 16 = one row,
#  so this version needs VLEN >= 128.
#
#  Registers:
#      s0  &A             s1  &B             s2  &C             s3  16 (matrix size)
#      t0  i              t4  k counter      t2  i * 64         t5  A[i][k], then &C[i][0]
#      a0  &A[i][k]       a1  &B[k][0]
#      v0-v3   row i of C (accumulator)      v8-v11  row k of B
#
#  Instructions executed (dynamic count):
#      setup  9   (3 x la = 6, li, vsetvli, li)
#      k loop 7 instructions x 16           = 112   per i
#      i body 5 before + 112 + 4 after      = 121   per i
#      total  9 + 16 x 121                  = 1,945
#
#  Result in `out` (0x10000000):
#      C[0][0..3] = -161, 259, 54, -27
#      C[15][12..15] = -188, 170, 113, -166
#
#  Run:  kites-cli benchmarks/programs/matmul_vector.s --stats --mem-dump 0x10000000 128
# ============================================================================================

        .text
        la      s0, A                   # s0 = &A[0][0]
        la      s1, B                   # s1 = &B[0][0]
        la      s2, out                 # s2 = &C[0][0]
        li      s3, 16                  # s3 = n
        vsetvli t0, s3, e32, m4, ta, ma # vl = 16: one row of 16 int32 in a group of 4 registers
        li      t0, 0                   # i = 0
iloop:
        vmv.v.i v0, 0                   # v0-v3 = C[i][0..15] = 0
        slli    t2, t0, 6               # t2 = i * 64 (bytes per row)
        add     a0, s0, t2              # a0 = &A[i][0]
        mv      a1, s1                  # a1 = &B[0][0]
        li      t4, 16                  # k counter
kloop:
        lw      t5, 0(a0)               # t5 = A[i][k]
        vle32.v v8, (a1)                # v8-v11 = B[k][0..15]
        vmacc.vx v0, t5, v8             # C[i][0..15] += A[i][k] * B[k][0..15]
        addi    a0, a0, 4               # next column of A: A[i][k+1]
        addi    a1, a1, 64              # next row of B: B[k+1][0]
        addi    t4, t4, -1
        bnez    t4, kloop
        add     t5, s2, t2              # t5 = &C[i][0]
        vse32.v v0, (t5)                # store row i of C
        addi    t0, t0, 1               # i++
        blt     t0, s3, iloop

        .data
out:    .zero   1024                    # C[16][16] (written by the program)

# Inputs: two 16x16 matrices of random int32 values in [-10, 10], one row per line.
A:      .word     -3,    1,    2,   -6,   -4,   -9,   -8,   -6,   -3,    6,   -4,    2,   10,  -10,    4,    5
        .word      4,    2,    5,    8,   -4,    2,   -8,    5,   -3,  -10,   -2,    6,    3,    5,    2,   -7
        .word     -2,   -7,   -8,    2,    9,    2,   -7,   -9,    0,   -3,   -8,    5,   10,    6,   -4,    8
        .word     -6,    9,   -8,    7,   -9,    5,   -4,   -6,    8,    4,    8,    4,   -1,    7,    1,    3
        .word     -6,   -5,    9,   -7,    7,    0,    1,   10,    5,    6,    9,   -4,   -1,   -6,    1,    6
        .word     -1,    6,   -8,    6,    7,   -3,    1,   -3,  -10,   -1,    0,   -3,   -2,   -9,    3,   -2
        .word      2,   -1,    3,   -5,    2,   -7,   -5,  -10,   -4,   -5,   -1,   -7,  -10,    2,    0,   -5
        .word      8,    3,   -3,   -6,    3,    8,    4,    3,    3,   -8,    8,   -8,   -1,   -7,   -9,   -8
        .word     -7,    6,    6,    1,   -6,    6,    5,   -6,    9,   -8,   -4,  -10,   -6,    0,    2,    8
        .word     -7,   -1,    1,    0,    1,  -10,   10,    5,   -4,   -9,    9,   -9,    8,   -5,   -5,    0
        .word      3,    4,   -7,   -8,   -4,    9,   -3,    5,    5,   -6,    2,   -7,   -7,    8,    4,   10
        .word     -6,    4,    6,   -8,    5,    8,    2,    1,    4,   -3,   -9,   -4,   -9,    4,   -7,   -7
        .word     -4,  -10,    9,    9,  -10,   10,    1,    3,    6,   -8,    9,    6,    8,   -4,    6,   -3
        .word      0,    6,    2,    6,   10,   -4,  -10,   -1,    9,   -1,   -8,    0,    2,   -3,   10,   -1
        .word     -8,    7,    3,    1,    0,    6,   -1,   10,    1,   -1,   10,   -7,   -4,    5,    1,    2
        .word      8,   -1,   -4,   -1,    2,    9,    2,    6,    1,    9,    8,    1,    3,   -4,    5,    6

B:      .word      6,   -8,   -2,   -5,   -8,    5,   -2,    3,   -7,    9,   -2,   -1,   -2,    6,   -2,   -2
        .word     10,    3,   -3,    1,   -3,   -3,    6,    4,   -3,   -3,    9,    6,   -4,    1,    5,   -7
        .word     -7,    2,   -8,   10,   -5,    8,    3,   -9,   -6,  -10,    9,   -4,   -2,   -8,   -7,    4
        .word      4,   -1,    8,   -8,   -6,    9,    1,   10,   -8,    0,   -9,   -8,   -6,   10,    1,   -8
        .word     -3,   -9,    9,    0,   -3,   -1,    1,    0,   -8,    6,   -7,    9,    1,    3,   -5,   -5
        .word     -2,    2,   -6,    3,   -8,   -7,    7,   -3,   -8,   -1,   -5,    0,   -4,   -7,   -9,   -9
        .word      7,   -5,   -5,    7,   10,    0,   -1,   -3,    6,    5,   -7,    8,   -8,   -6,   -6,   -3
        .word      0,   -4,    3,   -5,   -5,   -6,    4,   -9,    4,    8,    9,   -1,    3,    3,    8,    1
        .word      8,    5,    1,    6,   -5,   -1,   -2,    5,   -6,   -2,    5,   10,    6,    7,    6,   -1
        .word     -4,    8,    6,    2,   -6,    5,   10,    5,   -7,   -1,    2,   -9,   -9,    1,    6,   -1
        .word      1,    2,  -10,    9,   -6,  -10,   -8,   -5,    4,    8,   -6,    3,    8,   10,    8,   -7
        .word      1,    6,   -9,    9,   10,    1,    1,   -7,    0,   10,   -7,    5,   -5,   10,   -3,    5
        .word      1,    8,    1,    1,    3,    1,   10,   -9,   -6,   10,    0,   -9,   -9,   -3,    6,    1
        .word      5,    1,    5,    7,   -5,   -1,   -6,   10,   -2,  -10,    4,   -1,    8,   -2,    4,   -1
        .word     -1,    2,    5,    4,   10,   -8,    5,   10,    4,    0,   -6,    8,   -7,    1,    9,    4
        .word      1,    5,    8,    2,   -4,    7,    6,    8,   -5,   -6,    6,   -9,   -8,    7,   -1,   -4
