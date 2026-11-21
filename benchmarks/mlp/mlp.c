#include <stdint.h>
#include <stdio.h>

#include "mlp_weights.h"

/* y[j] = b[j] + sum_i x[i] * W[i][j]  for j = 0 .. n_out-1, then optionally ReLU and >> shift.
 * W is stored row by row, one row per input: W[i * n_out + j].
 * The vector version vectorises the j loop: vl neurons per pass, one vmacc.vx per input. */
static void dense(const int32_t *x, const int32_t *W, const int32_t *b, int32_t *y, int n_in, int n_out, int relu, int shift)
{
    for (int j = 0; j < n_out; j++) {           /* VECTORISED: vl neurons at a time */
        int32_t acc = b[j];
        for (int i = 0; i < n_in; i++)
            acc += x[i] * W[i * n_out + j];
        if (relu)
            acc = (acc > 0 ? acc : 0) >> shift; /* ReLU, then rescale */
        y[j] = acc;
    }
}

/* Index of the largest value; the first one wins a tie.
 * The vector version uses vredmax.vs, vmseq.vx and vfirst.m. */
static int argmax(const int32_t *v, int n)
{
    int cls = 0;                                /* VECTORISED: argmax */
    for (int k = 1; k < n; k++)
        if (v[k] > v[cls])
            cls = k;
    return cls;
}

/* One inference with the fixed weights of mlp_weights.h. */
int main(void)
{
    int32_t hidden[MLP_HID], logits[MLP_OUT];

    dense(input, W1, b1, hidden, MLP_IN, MLP_HID, 1, MLP_SHIFT); /* layer 1: 64 -> 32, ReLU */
    dense(hidden, W2, b2, logits, MLP_HID, MLP_OUT, 0, 0);       /* layer 2: 32 -> 10 */
    int cls = argmax(logits, MLP_OUT);

    printf("hidden");
    for (int j = 0; j < MLP_HID; j++)
        printf(" %d", (int)hidden[j]);
    printf("\nlogits");
    for (int k = 0; k < MLP_OUT; k++)
        printf(" %d", (int)logits[k]);
    printf("\npredicted digit %d (the image shows a %d)\n", cls, INPUT_LABEL);
    return 0;
}
