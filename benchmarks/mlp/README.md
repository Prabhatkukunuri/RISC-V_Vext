# MLP inference: scalar vs. vector

One inference of a small two-layer neural network (64 inputs → 32 hidden neurons with ReLU →
10 outputs) that recognises an 8×8 handwritten digit. The weights are fixed and pre-trained;
the programs only load them and run the inference. The same computation is written in C, in
scalar RISC-V assembly and in vector RISC-V assembly, and the two assembly versions are compared.

| File | What it is |
|---|---|
| `mlp_scalar.s` | Scalar program (RV64IM): code, weights and the input image in one file |
| `mlp_vector.s` | Vector program (RVV 1.0): the same data and memory layout |
| `mlp.c` | The same inference in C |
| `mlp_weights.h` | The weights, biases and input image used by `mlp.c` |
| `verify_mlp.py` | Runs all three, checks they agree, prints the comparison |

## Run

Each assembly file runs on its own (or open it in the GUI and press Run):

```
kites-cli benchmarks/mlp/mlp_scalar.s --stats --mem-dump 0x10000000 22
kites-cli benchmarks/mlp/mlp_vector.s --stats --mem-dump 0x10000000 22
```

Results in memory: `cls` (the predicted digit) at `0x10000000`, the 10 scores from `0x10000004`,
the 32 hidden values from `0x1000002c`. Both programs predict **2** (the image is a 2).

```
gcc -O2 -fwrapv -o mlp benchmarks/mlp/mlp.c && ./mlp
py -3.12 benchmarks/mlp/verify_mlp.py
```

## Result (single-cycle VM, VLEN = 128)

|  | Scalar | Vector | Speedup |
|---|---:|---:|---:|
| Instructions | 19,619 | 742 | **26.44×** |
| Instructions per multiply-add (2,368 per inference) | 8.29 | 0.31 | |
| Memory instructions | 4,831 | 198 | 24.40× |
| Bytes moved | 19,324 | 10,236 | 1.89× |
| Branches | 2,470 | 100 | 24.70× |
