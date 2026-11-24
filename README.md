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

### Windows (MSYS2): build, run and use

Everything below uses **PowerShell** and the free **MSYS2** toolchain (GCC + Qt 6). MSVC cannot be used,
because the ALU needs `__int128`.

#### 1. Install the tools (once)

1. Install MSYS2 from <https://www.msys2.org> into the default folder `C:\msys64`.
2. Open the **MSYS2 UCRT64** shell from the Start menu and install the compiler, Qt, CMake and Ninja:

   ```bash
   pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-qt6-base mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja
   ```

3. Install Git for Windows if you don't have it, and clone the repository:

   ```powershell
   git clone https://github.com/Prabhatkukunuri/RISC-V_Vext.git
   cd RISC-V_Vext
   ```

#### 2. Build (from the repository folder)

Every PowerShell window you use for Kites first needs the MSYS2 tools and DLLs on `PATH`:

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
```

Then configure and build the two variants, the GUI and the command-line tool with the tests. They are separate builds,
because `kites-cli` built together with the GUI crashes at start-up.

```powershell
# GUI: build\mingw-gui\Kites.exe
cmake -S . -B build/mingw-gui -G Ninja -DCMAKE_BUILD_TYPE=Debug -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64 -DQT_NO_PACKAGE_VERSION_CHECK=TRUE
cmake --build build/mingw-gui --target Kites

# Command line and tests: build\mingw-test\kites-cli.exe, build\mingw-test\kites_test.exe
cmake -S . -B build/mingw-test -G Ninja -DCMAKE_BUILD_TYPE=Debug -DDISABLE_GUI=ON -DENABLE_TESTS=ON -DCMAKE_PREFIX_PATH=C:/msys64/ucrt64 -DQT_NO_PACKAGE_VERSION_CHECK=TRUE
cmake --build build/mingw-test --target kites_cli kites_test
```

The `cmake -S ...` lines are needed only the first time. After pulling new changes, run only the
`cmake --build ...` lines. `QT_NO_PACKAGE_VERSION_CHECK` lets Kites build with MSYS2's Qt (6.7) although
`CMakeLists.txt` asks for 6.9.2.

#### 3. Open the simulator

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
.\build\mingw-gui\Kites.exe
```

Start it from PowerShell like this rather than by double-clicking `Kites.exe`: the window needs the Qt DLLs from
`C:\msys64\ucrt64\bin`.

#### 4. Write and run a program

1. **Editor tab:** type or paste a RISC-V program (`.data` and `.text` sections). Saving is not needed: **Run**
   assembles whatever is in the editor. **File → Open** loads a `.s` file (for example from `examples\`), and
   **File → Save** saves yours.
2. **Processor** (toolbar button): choose the processor:
   - *Single-cycle* (default): one instruction per step, the easiest to follow;
   - *5 stage Processor w/ hazard detection w/ forwarding*: the pipelined processor that also pipelines vector
     instructions. The other three 5-stage variants are there to show hazards without detection or forwarding.
3. Run it:

   | To | Do |
   |---|---|
   | run to the end | **Run** (▶) |
   | watch it slowly | set the delay box next to Run (milliseconds per instruction, e.g. `500`), then **Run** |
   | go one instruction at a time | **Run**, then **Pause**, then **Step** |
   | go back / forward | **Undo** / **Redo** (also for vector instructions) |

4. Look at the results:
   - **Vector tab:** registers `v0`–`v31` and the current `vl`, SEW, LMUL, VLEN and VLMAX;
   - **Memory tab:** **Go to .data** jumps to the data; bytes just written are highlighted;
   - **Processor tab:** the datapath of the chosen processor (vector wires light up green on the pipelined one);
   - **View → Pipeline Panel:** a cycle-by-cycle diagram of the pipeline (IF/ID/EX/MEM/WB, stalls shown as `nop`);
   - **Cache tab:** cache contents and hit/miss statistics, including the optional *Bicameral L1* page.

#### 5. Command line (no window)

```powershell
.\build\mingw-test\kites-cli.exe my_program.s --stats --mem-dump 0x10000040 4
.\build\mingw-test\kites-cli.exe my_program.s --vm rv5-h-f --stats   # on the pipelined processor
```

`--stats` prints the instruction count and cycles; `--mem-dump <address> <rows>` saves 8-byte rows of memory.
The registers, memory and errors are written to a `vm_state\` folder in the current folder. Other options:
`--vm rvss|rv5-nh-nf|rv5-h-nf|rv5-nh-f|rv5-h-f`, `--vlen <bits>`. See [CLI](#cli) and
[docs/CLI.md](docs/CLI.md).

Run the unit tests with:

```powershell
.\build\mingw-test\kites_test.exe --gtest_brief=1
```

#### 6. Troubleshooting

| Problem | Fix |
|---|---|
| "…dll was not found" when starting `Kites.exe` | Start it from PowerShell after `$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH`. |
| The build fails because `Kites.exe` cannot be written | Close the simulator first; Windows locks a running `.exe`. |
| After pulling new code, linking fails with `undefined reference to ...staticMetaObject` or `vtable for ...` | Qt's generated code is out of date. Delete `build\mingw-gui\Kites_autogen`, `build\mingw-gui\kites_core_autogen` and `build\mingw-gui\kites_cli_autogen`, then run the `cmake -S ...` and `cmake --build ...` lines for the GUI again. |
| `cmake` or `g++` is not recognised | The `$env:PATH` line was not run in this PowerShell window. |
| The program does not assemble | The error message gives the line number. Compare the syntax with the examples in `examples\`. |

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
