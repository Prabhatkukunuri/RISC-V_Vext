# KITES : RISC - V SIMULATOR

Kites is a RISC-V simulator and assembly code editor. It runs programs on a single-cycle processor and on four
5-stage pipelines (with or without hazard detection, with or without forwarding), and supports the I, M, F, D and
V extensions on all of them.

This fork adds the **RISC-V Vector extension (RVV 1.0)**, following the
[RVV 1.0 specification](https://github.com/riscvarchive/riscv-v-spec/blob/master/v-spec.adoc).
Vector instructions run on the single-cycle VM and on all four 5-stage pipeline variants, but only one
pipeline executes them in five stages:

| VM | How a vector instruction executes |
|---|---|
| Single cycle (RVSS) | the whole instruction in one cycle |
| 5-stage, hazard detection + forwarding (H_F) | **pipelined over IF/ID/EX/MEM/WB**: the result is computed in EX, vector loads/stores access memory in MEM, and vector registers and `vl`/`vtype` are written in WB. A vector forwarding path (EX/MEM → EX) and a vector hazard detection unit (`hdu_vector`) handle dependencies. |
| 5-stage, the other three variants (H_NF, NH_F, NH_NF) | the single-cycle vector unit, called in EX: the instruction's arithmetic, memory accesses and register writes all happen in the EX stage, and nothing vector-related passes through MEM or WB |

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
| Permutation | `vmv.x.s`, `vmv.s.x`, `vslideup`, `vslidedown`, `vslide1up`, `vslide1down`, `vrgather` (`.vv` / `.vx` / `.vi`), `vrgatherei16.vv`, `vcompress.vm`, `vmv1r.v` / `vmv2r.v` / `vmv4r.v` / `vmv8r.v` |
| Reductions | `vredsum`, `vredand`, `vredor`, `vredxor`, `vredmin(u)`, `vredmax(u)`, widening `vwredsum(u)` |
| Mask instructions | `vmand` / `vmnand` / `vmandn` / `vmor` / `vmnor` / `vmorn` / `vmxor` / `vmxnor`, `vcpop.m`, `vfirst.m`, `vmsbf.m`, `vmsif.m`, `vmsof.m`, `viota.m`, `vid.v` |
| Loads and stores | listed below |

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

## Bicameral Cache

The L1 data cache can be replaced by the Bicameral Cache, after *The Bicameral Cache: a split cache for vector
architectures* (arXiv 2407.15440). The L1 is split into two halves of equal size:

| Half | Organisation | Used by |
|---|---|---|
| Scalar Cache | set associative, block = one sector | scalar loads and stores |
| Vector Cache | fully associative; each line holds several sectors | vector loads and stores |

A vector access whose sector is in the Scalar Cache moves it to the Vector Cache; a scalar access whose sector is
in the Vector Cache is served there. A sector is never in both halves. Both halves use LRU replacement and
are write-back (the GUI can switch them to write-through).

- **GUI:** Cache tab → **Bicameral L1**: tick **Enable**, choose the L1 size, sector size, sectors per Vector Cache
  line and Scalar Cache associativity, and press **Apply**. The page shows both halves and their statistics.
- **CLI:** `--bicameral` uses an 8 KB L1 (Scalar Cache 16 indices × 4 ways × 64 B, Vector Cache 4 lines × 16
  sectors × 64 B); `--stats` prints its hit, miss, migration and write-back counts.

[docs/EXAMPLES.md](docs/EXAMPLES.md) walks through two example programs, and [docs/CLI.md](docs/CLI.md#bicameral-cache---bicameral)
has the details.

## Building

Kites needs Qt 6.9 or newer and a GCC or Clang compiler (MSVC does not support the `__int128` the ALU uses).
The GUI and the headless `kites-cli` + tests (`-DDISABLE_GUI=ON -DENABLE_TESTS=ON`) must be separate builds:
`kites-cli` crashes at start-up if built with the GUI enabled.

### macOS with Homebrew

1. Install the toolchain (Apple Clang comes with the Xcode command line tools; Homebrew's Qt is newer than
   the 6.9.2 that `CMakeLists.txt` asks for):

   ```bash
   xcode-select --install
   brew install qt cmake ninja
   ```

2. Build from the repository root. On an Intel Mac, Homebrew lives in `/usr/local`, so use
   `-DCMAKE_PREFIX_PATH=/usr/local` instead.

   ```bash
   # GUI
   cmake -S . -B build-gui -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/opt/homebrew
   cmake --build build-gui --target Kites

   # Headless CLI and tests
   cmake -S . -B build -G Ninja -DDISABLE_GUI=ON -DENABLE_TESTS=ON -DCMAKE_PREFIX_PATH=/opt/homebrew
   cmake --build build --target kites_cli kites_test
   ```

3. Run:

   ```bash
   open build-gui/Kites.app
   ./build/kites-cli examples/vector_add.s --vm rv5-h-f --stats --mem-dump 10000050 5
   ./build/test/kites_test --gtest_brief=1
   ```

   On macOS, `kites-cli` writes its dumps to `~/Library/Application Support/Kites/vm_state/`.

### Linux

1. Install a compiler, CMake, Ninja and Qt 6 (on Ubuntu/Debian):

   ```bash
   sudo apt install build-essential cmake ninja-build qt6-base-dev
   ```

2. Build from the repository root. `QT_NO_PACKAGE_VERSION_CHECK` lets CMake accept a distribution Qt
   older than the 6.9.2 that `CMakeLists.txt` asks for.

   ```bash
   # GUI
   cmake -S . -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=Release -DQT_NO_PACKAGE_VERSION_CHECK=TRUE
   cmake --build build/gui --target Kites

   # Headless CLI
   cmake -S . -B build/cli -G Ninja -DCMAKE_BUILD_TYPE=Release -DDISABLE_GUI=ON -DQT_NO_PACKAGE_VERSION_CHECK=TRUE
   cmake --build build/cli --target kites_cli
   ```

3. Optionally, install both on your `PATH` (`~/.local/bin`):

   ```bash
   mkdir -p ~/.local/bin
   cp build/gui/Kites ~/.local/bin/Kites-vext
   cp build/cli/kites-cli ~/.local/bin/kites-cli-vext
   ```

4. Run:

   ```bash
   Kites-vext &
   kites-cli-vext examples/vector_add.s --vm rv5-h-f --stats
   ```

   On Linux, `kites-cli` writes its dumps to `./vm_state/` in the current directory.

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
   build/mingw-test/kites-cli.exe examples/vector_add.s
   build/mingw-test/kites_test.exe --gtest_brief=1
   ```

   Close the simulator before rebuilding it; Windows locks a running `.exe`.

## Examples

See [docs/EXAMPLES.md](docs/EXAMPLES.md) for example programs that show the pipeline (stalls, forwarding and
flushes, for scalar and vector instructions) and the Bicameral Cache in the GUI.

## Testing

See [docs/TESTING.md](docs/TESTING.md) for how to build and run tests, including coverage reports.

The vector extension has two suites, sharing `test/vector_test_helpers.h`:

| File | Covers |
|---|---|
| `test/vector_rvss_tests.cpp` | the single-cycle VM: register file, CSRs and VLEN; `vset*`; every integer instruction group (arithmetic, widening/extension/logical, shifts, compares, multiply/divide, carry, permutation, reductions, masks); all loads and stores; the staged EX/MEM/WB split; spec-rule sweeps; encodings; edge-value sweeps; the spec's code examples; masking/tail/vl corner cases |
| `test/vector_rv5s_tests.cpp` | the 5-stage pipelines: each instruction group on all four variants against RVSS, scalar/vector dependencies, the pipelined vector unit of H_F (forwarding, `hdu_vector`, CSR writes in WB, datapath view), edge-value sweeps and spec examples on H_F/H_NF, and random dependent programs against RVSS |

The full suite has 867 tests.

## CLI

`kites-cli` assembles a program and runs it without the GUI, writing the registers, disassembly, errors and any
requested memory to `vm_state/` (on macOS, `~/Library/Application Support/Kites/vm_state/`). See
[docs/CLI.md](docs/CLI.md) for details.

```bash
kites-cli <file.s> [--vm <name>] [--mem-dump <hex_addr> <rows>]... [--vlen <bits>] [--stats] [--bicameral]
kites-cli [--repl | -i]
```

| Option | Effect |
|---|---|
| `--vm rvss\|rv5-nh-nf\|rv5-h-nf\|rv5-nh-f\|rv5-h-f` | pick the VM (default `rvss`); all five run to completion in batch mode |
| `--mem-dump <hex_addr> <rows>` | dump `<rows>` 8-byte rows of memory from `<hex_addr>` after the run; repeatable |
| `--vlen <bits>` | vector register length (default 128) |
| `--stats` | print run statistics and write `vm_state/stats_dump.json`: cycles, instructions retired, and on `rvss` the instruction mix, bytes moved and vector elements processed |
| `--bicameral` | use the [Bicameral Cache](#bicameral-cache) as the L1 data cache |
| `--repl`, `-i` | open the interactive REPL instead of running a file |

## Known limitations

- **Not implemented yet:** fixed-point (saturating, averaging, `vsmul`, scaling shifts, `vnclip`) and all vector
  floating-point instructions, including the floating-point permutations `vfmv.f.s`, `vfmv.s.f`,
  `vfslide1up.vf` and `vfslide1down.vf`.
- **Cache tab:** typing a very large set count into the cache configuration can crash the GUI.
- **No `csrr` pseudo-instruction:** read CSRs with `csrrs rd, vl, x0`.
- **Register names:** `v0`–`v31` cannot be used as label names.
