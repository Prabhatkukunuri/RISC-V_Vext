# KITES : RISC - V SIMULATOR

Kites a is RISC V simulator and assembly code editor built for RISC V ISA.
Kites support I,M,D and F extenstion in single cyle mod and I,M extension in 5 Stage pipelining mode.(D,F will be added in future releases).

This fork adds the **RISC-V Vector extension (RVV 1.0)**, following the
[RVV 1.0 specification](https://github.com/riscvarchive/riscv-v-spec/blob/master/v-spec.adoc),
on the single-cycle VM and all four 5-stage pipeline variants.

## Vector extension (RVV 1.0)

### What is supported

| Group | Instructions |
|---|---|
| Configuration | `vsetvli`, `vsetivli`, `vsetvl` (every SEW `e8`–`e64` and LMUL `mf8`–`m8`) |
| Integer add/subtract | `vadd`, `vsub`, `vrsub` (`.vv` / `.vx` / `.vi` forms) |
| Widening add/subtract | `vwaddu`, `vwadd`, `vwsubu`, `vwsub` (`.vv` / `.vx` / `.wv` / `.wx`) |
| Integer extension | `vzext.vf2/.vf4/.vf8`, `vsext.vf2/.vf4/.vf8` |
| Bitwise logical | `vand`, `vor`, `vxor` (`.vv` / `.vx` / `.vi`) |
| Shifts | `vsll`, `vsrl`, `vsra`; narrowing `vnsrl`, `vnsra` (`.wv` / `.wx` / `.wi`) |
| Add-with-carry | `vadc`, `vsbc`, `vmadc`, `vmsbc` |
| Compare (write a mask) | `vmseq`, `vmsne`, `vmslt(u)`, `vmsle(u)`, `vmsgt(u)`, plus the `vmsge(u)` / `vmsgt.vv` aliases |
| Min/max | `vmin(u)`, `vmax(u)` |
| Multiply/divide | `vmul`, `vmulh(u/su)`, `vdiv(u)`, `vrem(u)`, widening `vwmul(u/su)` |
| Multiply-add | `vmacc`, `vnmsac`, `vmadd`, `vnmsub`, widening `vwmacc(u/su/us)` |
| Merge/move | `vmerge`, `vmv.v.v` / `.v.x` / `.v.i` |
| Permutation (section 16) | `vmv.x.s`, `vmv.s.x`, `vslideup`, `vslidedown`, `vslide1up`, `vslide1down`, `vrgather` (`.vv` / `.vx` / `.vi`), `vrgatherei16.vv`, `vcompress.vm`, `vmv1r.v` / `vmv2r.v` / `vmv4r.v` / `vmv8r.v` |
| Reductions (section 14) | `vredsum`, `vredand`, `vredor`, `vredxor`, `vredmin(u)`, `vredmax(u)`, widening `vwredsum(u)` |
| Mask instructions (section 15) | `vmand` / `vmnand` / `vmandn` / `vmor` / `vmnor` / `vmorn` / `vmxor` / `vmxnor`, `vcpop.m`, `vfirst.m`, `vmsbf.m`, `vmsif.m`, `vmsof.m`, `viota.m`, `vid.v` |
| Loads and stores | the complete set from spec section 7 (316 mnemonics), listed below |

Loads and stores cover every addressing mode:

- **Unit-stride:** `vle<eew>.v` / `vse<eew>.v`, e.g. `vle32.v v1, (a0)`.
- **Strided:** `vlse<eew>.v` / `vsse<eew>.v` with a signed byte stride in a register (negative and zero strides work).
- **Indexed (gather/scatter):** `vluxei` / `vloxei` / `vsuxei` / `vsoxei`, with zero-extended byte offsets in a vector register.
- **Mask:** `vlm.v` / `vsm.v`.
- **Fault-only-first:** `vle<eew>ff.v`. A fault after element 0 shrinks `vl` instead of trapping.
- **Segments (2–8 fields):** e.g. `vlseg3e8.v` splits packed RGB data into three registers.
- **Whole-register:** `vl<n>re<eew>.v` / `vs<n>r.v`. These ignore `vtype` and `vl`.

Common behaviour:

- **Machine parameters:** VLEN is configurable (default 128; any power of two from 64 to 65536). ELEN is 64.
- **Masking:** any instruction can take a trailing `, v0.t`.
- **Tail and masked-off elements:** left undisturbed, as Spike does.
- **Illegal configurations trap:** for example running a vector instruction before any `vsetvli`, a misaligned register group, an EMUL outside 1/8–8, or a masked instruction writing `v0`.
- **Undo/redo:** works for vector registers, vector CSRs and memory written by vector stores.
- **Misaligned elements** (spec section 8) are transferred, the same way scalar misaligned loads and stores are.

### Vector tab

The **Vector** entry at the bottom of the sidebar shows the vector state while a program runs:

- **CSR strip:** VLEN, the decoded `vtype` (e.g. `e32, m2, tu, mu`), `vl`, VLMAX and `vstart`.
- **Register grid:** `v0`–`v31`, one column per element (element 0 first). The element width follows the current SEW or can be fixed to `e8`–`e64`. Values can be shown in hex, unsigned, signed or binary.
- **Elements changed by the last vector instruction** are highlighted.
- **Tail elements** (index ≥ `vl`) are dimmed.
- **LMUL register groups** are shaded alternately.
- **Hovering over `v0`** shows it as a mask.

The editor highlights and autocompletes the vector mnemonics, `v0`–`v31` and `v0.t`.

The **Memory** tab highlights the bytes written by the latest store (scalar or vector). **Go to .data** jumps to the
start of the data section, **Go to last write** to the latest store, and the rows can be listed in ascending or
descending address order.

### Running a program step by step

**Run** executes the program at the speed set in the box next to it (milliseconds per instruction). **Pause** stops
it; while paused, **Step** executes one instruction, **Undo** / **Redo** move back and forward, and **Resume**
continues the run. The editor highlights the current line throughout.

### Example

```asm
.data
nums:   .word 10, 20, 30, 40, 50, 60, 70, 80     # 0x10000000

.text
    lui      a0, 0x10000          # a0 = &nums (.data starts at 0x10000000)
    vsetivli t0, 4, e32, m1, ta, ma
    vle32.v  v1, (a0)             # v1 = [10, 20, 30, 40]
    addi     t1, a0, 16
    vle32.v  v2, (t1)             # v2 = [50, 60, 70, 80]
    vadd.vv  v3, v1, v2           # v3 = [60, 80, 100, 120]
    li       t2, 8
    vlse32.v v4, (a0), t2         # every other word: v4 = [10, 30, 50, 70]
```

A longer example covering every kind of load and store, with the expected result written next to each
instruction, is in [`examples/vector_demo.s`](examples/vector_demo.s).
[`examples/vector_gui_walkthrough.s`](examples/vector_gui_walkthrough.s) is meant for the GUI: it covers loads,
compares, widening/narrowing, permutations and stores, also with the expected results written next to each line.

### Checking results in the dump files

Kites writes its state to a `vm_state/` folder inside the directory it is started from:

| File | Contents |
|---|---|
| `registers_dump.json` | all registers; vector registers are under `"vec_registers"` |
| `memory_dump.json` | memory rows requested with `kites-cli --mem-dump <hex_addr> <rows>` |
| `errors_dump.json` | assembler errors (line and message) |
| `disassembly.txt` | the assembled program |

Each vector register is printed as one hex number with the most significant byte first, so element 0 is in the
**rightmost** digits. For example, after the program above,
`"v1" : "0x000000280000001e000000140000000a"` read in 8-digit (e32) groups from the right is `[10, 20, 30, 40]`.

For example, to run the demo and dump 38 rows of memory from the start of `.data`:

```bash
kites-cli examples/vector_demo.s --mem-dump 0x10000000 38
```

## Downloading and Installation

Prebuilt libraries are available for Linux,Windows on [Release Page](https://github.com/hghit-harshit/Kites/releases)

### Ubuntu(for only ubuntu-22.04 and above)
- Run chmod +x for the Kites AppImage.
- Run the file.

### Windows
- All the required files are inside the zip file, so you can just run the file.
- In case you get popup regarding some missing dll please contact us.

## MacOS
- Put Kites in the application 

RUN

```bash
sudo xattr -rd com.apple.quarantine sudo xattr -rd com.apple.quarantine /path/to/application.app

```

## Building

To build Kites you will first neet to install Qt(version above 6.9)
Then simply build using

```bash
cmake -DCMAKE_PREFIX_PATH="path/to/Qt/6.x.x/<compiler>" ..
```

Kites needs a GCC or Clang compiler (the ALU uses `__int128`, which MSVC does not support).

There are two configurations. They have to be separate builds because `kites-cli` crashes at start-up if it is built
with the GUI enabled:

| Build | Options | Produces |
|---|---|---|
| GUI | (defaults) | `Kites`: the simulator window |
| Headless | `-DDISABLE_GUI=ON -DENABLE_TESTS=ON` | `kites-cli` and the test binary `kites_test` |

### Windows with MSYS2

1. Install the toolchain from an MSYS2 shell:

   ```bash
   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
   ```

2. Build from the repository root, with `C:\msys64\ucrt64\bin` on `PATH`:

   ```bash
   export PATH="/c/msys64/ucrt64/bin:$PATH"

   # GUI
   cmake -S . -B build/mingw-gui -G Ninja -DCMAKE_BUILD_TYPE=Debug \
         -DCMAKE_PREFIX_PATH=/c/msys64/ucrt64 -DQT_NO_PACKAGE_VERSION_CHECK=TRUE
   cmake --build build/mingw-gui --target Kites

   # Headless CLI and tests
   cmake -S . -B build/mingw-test -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDISABLE_GUI=ON -DENABLE_TESTS=ON \
         -DCMAKE_PREFIX_PATH=/c/msys64/ucrt64 -DQT_NO_PACKAGE_VERSION_CHECK=TRUE
   cmake --build build/mingw-test --target kites_cli kites_test
   ```

   `QT_NO_PACKAGE_VERSION_CHECK` is needed only when MSYS2's Qt is older than the 6.9.2 that `CMakeLists.txt`
   asks for; Kites builds and runs with Qt 6.7.

3. Run with the same `PATH`, so Windows finds the Qt DLLs:

   ```bash
   build/mingw-gui/Kites.exe &
   build/mingw-test/kites-cli.exe examples/vector_demo.s
   build/mingw-test/kites_test.exe --gtest_brief=1
   ```

   Close the simulator before rebuilding it; Windows locks a running `.exe`.

## Testing

See [docs/TESTING.md](docs/TESTING.md) for how to build and run tests, including coverage reports.

The vector extension has its own suites:

| File | Covers |
|---|---|
| `test/vector_register_file_test.cpp` | register file, CSRs, VLEN configuration |
| `test/vset_test.cpp` | `vsetvli` / `vsetivli` / `vsetvl` |
| `test/vector_int_arith_test.cpp` | `vadd` / `vsub` / `vrsub` |
| `test/vector_widen_ext_logic_test.cpp` | widening, extension and logical instructions |
| `test/vector_shift_test.cpp`, `test/vector_narrow_shift_test.cpp` | single-width and narrowing shifts |
| `test/vector_compare_test.cpp` | compares and their aliases |
| `test/vector_mul_div_test.cpp` | min/max, multiply, divide, multiply-add, merge/move |
| `test/vector_carry_test.cpp` | add-with-carry / subtract-with-borrow |
| `test/vector_load_store_test.cpp` | all loads and stores, including misaligned elements |
| `test/vector_permute_test.cpp` | permutation instructions |
| `test/vector_reduction_test.cpp`, `test/vector_mask_test.cpp` | reductions and mask instructions |
| `test/vector_spec_sweep_test.cpp` | spec-wide sweep of the arithmetic instructions |

Encodings are checked against LLVM (`llvm-mc`) and the official `riscv-opcodes` tables, and every program runs on
all five VMs. The full suite has 332 tests, all passing.

## CLI

See [docs/CLI.md](docs/CLI.md) for how to build and use the headless command-line executable. Options added for
the vector work:

| Option | Effect |
|---|---|
| `--vm rvss\|rv5-nh-nf\|rv5-h-nf\|rv5-nh-f\|rv5-h-f` | pick the VM; all five now run to completion in batch mode |
| `--vlen <bits>` | vector register length (default 128) |
| `--stats` | print run statistics and write `vm_state/stats_dump.json`: cycles, instructions retired, and on `rvss` the instruction mix, bytes moved and vector elements processed |

## Scalar vs. vector benchmarks

`benchmarks/` compares five common operations, each written in assembly twice: as a plain scalar RV64IM loop and
as an RVV vector loop. Both versions read the same data, and each run is checked for the exact expected output.
Speedup is measured in assembly instructions executed (scalar / vector, on the single-cycle VM, VLEN = 128):

| Operation | Scalar | Vector | Speedup |
|---|---:|---:|---:|
| Vector add (256 elements) | 2,311 | 95 | 24.3x |
| AXPY, out = 3x + y (256 elements) | 2,568 | 96 | 26.8x |
| Dot product (256 elements) | 2,057 | 91 | 22.6x |
| Array maximum (256 elements) | 1,289 | 66 | 19.5x |
| Matrix multiply (16x16) | 35,640 | 1,945 | 18.3x |

```bash
python benchmarks/run_benchmarks.py          # finds build/*/kites-cli; needs matplotlib for the chart
```

The programs in `benchmarks/programs/` are hand-written and commented (C equivalent, register map, instruction-count
derivation and expected result in each header); they also open in the GUI. The script checks every result and writes
`benchmarks/results/` (RESULTS.md, summary.csv, results.json, speedup.png). The metrics and their analysis are in
[benchmarks/REPORT.md](benchmarks/REPORT.md).

## Known limitations

- **Not implemented yet:** fixed-point (saturating, averaging, `vsmul`, scaling shifts, `vnclip`) and all vector
  floating-point instructions, including the floating-point permutations `vfmv.f.s`, `vfmv.s.f`,
  `vfslide1up.vf` and `vfslide1down.vf`.
- **CSR instructions:** not executed by the pipelined VMs.
- **Cache tab:** typing a very large set count into the cache configuration can crash the GUI.
- **No `csrr` pseudo-instruction:** read CSRs with `csrrs rd, vl, x0`.
- **Register names:** `v0`–`v31` cannot be used as label names.
