# Using the Kites CLI

`kites-cli` is a headless build of the Kites simulator: it assembles a RISC-V
`.s` file, runs it on the chosen VM, and writes the results (registers,
disassembly, and optionally a memory dump) to disk. It needs no display and no
`QApplication` — it links the same `kites_core` the GUI uses, built with
`DISABLE_GUI` defined.

> **Status:** the CLI assembles and runs a program end-to-end and dumps
> register/disassembly/memory state. `--repl` (or running with no file) opens an
> interactive REPL driven by `command_handler`.

## Building

The CLI target is built automatically alongside the GUI app; no extra CMake
option is required.

```bash
cmake -B build -DCMAKE_PREFIX_PATH="path/to/Qt/6.x.x/<compiler>" .
cmake --build build --target kites_cli
```

The resulting binary is `kites-cli` (`build/kites-cli`, or
`build/<config>/kites-cli.exe` on multi-config generators like Visual Studio).

## Usage

```
kites-cli <assembly-file> [--vm <name>] [--mem-dump <hex_addr> <row_count>]... [--vlen <bits>] [--stats] [--bicameral]
kites-cli [--repl | -i]
```

| Argument       | Description                                                                          |
|----------------|---------------------------------------------------------------------------------------|
| `<assembly-file>` | Path to the `.s` file to assemble and run. Required unless `--repl` is used.     |
| `--vm <name>`  | Which VM to run on (see table below). Defaults to `rvss`.                            |
| `--mem-dump <hex_addr> <row_count>` | Dump `<row_count>` 8-byte rows of memory starting at `<hex_addr>` after the run. Repeatable — pass it multiple times to dump several regions in one invocation. |
| `--vlen <bits>` | Vector register length VLEN in bits (default 128). A power of two from 64 to 65536. Changes VLMAX, so how many elements each `vsetvli` strip handles. |
| `--stats`      | Print run statistics and write `stats_dump.json`: cycles, instructions retired and CPI on every VM; on `rvss` also the instruction mix (scalar and vector), vector elements processed and bytes loaded/stored. |
| `--bicameral`  | Use the Bicameral Cache as the L1 data cache (see below). |
| `--repl`, `-i` | Open the interactive REPL instead of running a file.                                  |

### `--vm` values

| Name        | `ProcessorType`         | Description                                   |
|-------------|-------------------------|------------------------------------------------|
| `rvss`      | `RVSS`                  | Single-cycle (default). Supports I, M, D, F and V. |
| `rv5-nh-nf` | `RV5Stage_NH_NF`        | 5-stage pipeline, no hazard detection, no forwarding. |
| `rv5-h-nf`  | `RV5Stage_H_NF`         | 5-stage pipeline, hazard detection, no forwarding. |
| `rv5-nh-f`  | `RV5Stage_NH_F`         | 5-stage pipeline, no hazard detection, forwarding. |
| `rv5-h-f`   | `RV5Stage_H_F`          | 5-stage pipeline, hazard detection, forwarding. |

All five VMs run vector (V) instructions; see [Running vector programs](#running-vector-programs) for how each one
executes them.

## Example

```bash
cat > /tmp/hello.s <<'EOF'
li   a0, 42       # value to print
li   a7, 1        # SYSCALL_PRINT_INT
ecall

li   a0, 0        # exit code
li   a7, 10       # SYSCALL_EXIT
ecall
EOF

./build/kites-cli /tmp/hello.s --vm rv5-h-f
```

Sample output:

```
[Syscall output: 42]
Exited with exit code: 0
Assembled '/tmp/hello.s': 4 instruction word(s), 0 data item(s).
Run completed.
Registers   -> /tmp/vm_state/registers_dump.json
Disassembly -> /tmp/vm_state/disassembly.txt
Errors      -> /tmp/vm_state/errors_dump.json
```

Dumping a region of memory in the same run:

```bash
./build/kites-cli /tmp/hello.s --mem-dump 0 4 --mem-dump 100 2
```

adds a `Memory -> .../vm_state/memory_dump.json` line and writes both
requested regions (4 rows starting at address `0x0`, then 2 rows starting at
`0x100`) to that file.

## Running vector programs

### How each VM executes a vector instruction

| VM | Execution |
|---|---|
| `rvss` | the whole instruction in one cycle |
| `rv5-h-f` | pipelined over IF/ID/EX/MEM/WB: computed in EX, vector loads/stores access memory in MEM, vector registers and `vl`/`vtype` written in WB. A vector forwarding path and a vector hazard detection unit handle dependencies (a vector load followed by a reader of its registers costs one stall). |
| `rv5-h-nf`, `rv5-nh-f`, `rv5-nh-nf` | the single-cycle vector unit, called in EX: the whole instruction (arithmetic, memory accesses, register writes) happens in the EX stage |

`rv5-nh-f` and `rv5-nh-nf` have no hazard detection, so programs for them need dependent instructions spaced apart by
hand (a `nop` after a load-use pair on `rv5-nh-f`; after every dependency on `rv5-nh-nf`), for vector and scalar
code alike.

### Seeing vector results

`registers_dump.json` holds the vector CSRs (`vl`, `vtype`, `vstart`, `vlenb`) and every vector register,
`v0`–`v31`, under `vec_registers`. Each register is one hex string of VLEN bits, most-significant byte first, so
element 0 is at the **right** end. After the vector-add example below (VLEN = 128, `e32`), the dump has
`"v3": "0x000000580000004d0000006e00000063"`: elements 0 and 1 are 0x63 (99) and 0x6e (110) from the last strip
(`vl` = 2), and elements 2 and 3 still hold 0x4d (77) and 0x58 (88) from the strip before, because Kites leaves
tail elements undisturbed. The dump shows the registers after the last instruction, so a strip-mined loop leaves
only its last strip there.

To check a whole result array, store it to memory with `vse<eew>.v` and dump that memory with `--mem-dump`. The
data section starts at `0x10000000`, so a label's address is `0x10000000` plus its offset in `.data`.

### Example: strip-mined vector add

[`examples/vector_add.s`](../examples/vector_add.s) computes `c[i] = a[i] + b[i]` for 10 words with a strip-mined
loop. `c` follows the two 40-byte arrays in `.data`, so it starts at `0x10000000 + 80 = 0x10000050`:

```asm
loop:
    vsetvli t0, a0, e32, m1, ta, ma   # t0 = vl = min(n, VLMAX), 32-bit elements
    vle32.v v1, (a1)          # v1 = a[0 .. vl-1]
    vle32.v v2, (a2)          # v2 = b[0 .. vl-1]
    vadd.vv v3, v1, v2        # v3 = v1 + v2
    vse32.v v3, (a3)          # c[0 .. vl-1] = v3, so --mem-dump can show the result
    slli  t1, t0, 2           # bytes processed = vl * 4
    add   a1, a1, t1
    add   a2, a2, t1
    add   a3, a3, t1
    sub   a0, a0, t0          # n -= vl
    bnez  a0, loop
```

Run it on two VMs from the project root

```bash
./build/kites-cli examples/vector_add.s --vm rvss    --stats --mem-dump 10000050 5
./build/kites-cli examples/vector_add.s --vm rv5-h-f --stats --mem-dump 10000050 5
```

Both runs write the same `memory_dump.json`: `c` = 11, 22, …, 110, two 32-bit elements per 8-byte row (little
endian, so the lower-addressed element is the low half):

```json
{
    "0x0000000010000050": "0x000000160000000b",
    "0x0000000010000058": "0x0000002c00000021",
    ...
}
```

The `--stats` summaries:

```
Statistics (rvss, VLEN=128):
  cycles 40, instructions retired 40, CPI 1.000
  scalar: 22 alu, 0 load/store, 3 branch (2 taken), 0 jump, 0 other
  vector: 15 instructions (3 vset, 9 load/store, 3 compute), 40 elements
  memory: 80 bytes loaded, 40 bytes stored

Statistics (rv5-h-f, VLEN=128):
  cycles 53, instructions retired 40, CPI 1.325
```

On `rv5-h-f` the 13 extra cycles are the 4-cycle pipeline fill, one stall per strip for `vle32.v v2` → `vadd.vv`
(3), and 3 cycles for each of the 2 taken branches. With VLEN = 128, `e32`/`m1` gives VLMAX = 4, so the loop runs
3 strips (4 + 4 + 2) and `vl` ends at 2. With `--vlen 256`, VLMAX = 8 and it runs 2 strips (8 + 2).

### Limits

- Integer vector instructions only: vector floating-point and fixed-point (saturating, averaging, `vsmul`, scaling
  shifts, `vnclip`) instructions are not implemented.
- There is no `csrr` pseudo-instruction: read `vl`/`vtype` with `csrrs rd, vl, x0`.

## Bicameral Cache (`--bicameral`)

`--bicameral` replaces the L1 data cache with the Bicameral Cache
(`src/processor/cache/bicameral/`), after *The Bicameral Cache: a split cache for vector
architectures* (arXiv 2407.15440). It is an 8 KB L1 split into two halves:

| Half | Organisation | Used for |
|------|--------------|----------|
| Scalar Cache (SC) | 16 indices x 4 ways x 64 B lines = 4 KB | scalar loads and stores |
| Vector Cache (VC) | fully associative, 4 lines x 1 KB, each line 16 sectors of 64 B = 4 KB | vector loads and stores (`vle*`/`vse*`) |

- A VC miss is either a **sector miss** (the line is there, the 64 B sector isn't: fetch that
  sector, evict nothing) or a **line miss** (no line for the 1 KB block: evict a whole line,
  writing back only its dirty sectors, then fetch one sector).
- **Cross lookup, one-sided migration**: a vector access whose sector is in the SC moves it to the
  VC; a scalar access whose sector is in the VC is served there and stays there. A sector is never
  in both halves.
- The two halves are always the same size, and the SC block size is the sector size. In the GUI
  (Cache tab → Bicameral L1) you pick the L1 size, the sector size, the sectors per VC line and the
  SC associativity; the SC index count and the VC line count are derived from them.
- Both halves are write-back and write-allocate with LRU replacement. Every transfer between L1
  and L2 is one 64 B sector. L2 is enlarged, if needed, to at least 8 KB with 64 B lines.
- Before `--mem-dump`, all dirty data is written back to main memory so the dump shows what the
  program wrote.

With `--stats`, the summary gets a Bicameral Cache section:

```
Bicameral Cache (L1 8 KB = SC 4 KB + VC 4 KB; L2 8 KB)
  Scalar Cache : 0 hits, 0 misses, 0 write-backs
  Vector Cache : 28 hits, 1 sector misses, 1 line misses, 1 sector write-backs
  Cross lookup : 0 SC->VC migrations, 0 scalar cross-hits in the VC
```

## Program output during the run

Programs that execute `ecall` for I/O print directly to the CLI's stdout
while running, wrapped in a marker so it's easy to spot in mixed output:

```
[Syscall output: <value or string>]
```

`PRINT_INT` (`a7=1`), `PRINT_FLOAT` (`a7=2`), `PRINT_DOUBLE` (`a7=3`), and
`PRINT_STRING` (`a7=4`) all print this way; `EXIT` (`a7=10`) prints
`Exited with exit code: <code>`.

**Known limitation:** a program that hits a blocking `READ` syscall (`a7=63`)
will hang forever — the CLI doesn't yet feed terminal stdin into the VM's
input queue. Avoid programs that read from stdin until this is implemented.

## Output files

All state is written under a `vm_state/` directory rather than to stdout, so
it can be consumed by scripts or other tooling. The directory location
depends on platform:

- **Windows / Linux:** `<current working directory>/vm_state/`
- **macOS:** `~/Library/Application Support/Kites/vm_state/`

| File                    | Written                        | Contents                                                     |
|-------------------------|---------------------------------|----------------------------------------------------------------|
| `disassembly.txt`       | Always, on successful assembly  | Human-readable disassembly with address labels and raw hex words. |
| `errors_dump.json`      | Always                          | `{"errorCode": 0, "errors": []}` on success, or a list of `{line, message}` objects on assembly failure. |
| `registers_dump.json`   | Always, after the run           | CSRs (including `vl`, `vtype`, `vstart`, `vlenb`), all 32 GPRs (`x0`-`x31`), all 32 FPRs (`f0`-`f31`) and all 32 vector registers (`v0`-`v31`, VLEN bits each), each as a hex string. |
| `memory_dump.json`      | Only if `--mem-dump` was passed | One entry per 8-byte word in the requested address ranges, keyed by hex address, e.g. `"0x0000000000000000": "0x002a000000000000"`. |

### `registers_dump.json` shape

```json
{
    "control and status registers": {
        "...": "0x..."
    },
    "gp_registers": {
        "x0" : "0x0000000000000000",
        "x1" : "0x0000000000000000",
        ...
    },
    "fp_registers": {
        "f0" : "0x0000000000000000",
        ...
    },
    "vec_registers": {
        "v0" : "0x00000000000000000000000000000000",
        ...
    }
}
```

## Exit codes

| Code | Meaning                                                             |
|------|----------------------------------------------------------------------|
| `0`  | Assembled and ran successfully.                                      |
| `1`  | Bad invocation (missing file argument, unknown `--vm` value, unknown flag) or assembly failed — see `errors_dump.json` and stderr. |
| `2`  | Assembly succeeded but the run raised a runtime error — see stderr for the message. |
| `3`  | The run succeeded but writing the `--mem-dump` output failed (e.g. malformed hex address). |

## Notes

- `--vm` and `--mem-dump` may appear in any order after the file argument,
  and `--mem-dump` may be repeated.
- Instruction step delay (used to pace the GUI's per-instruction animation)
  is forced to `0`, so the CLI runs a program at full speed regardless of the
  GUI's configured delay.
- See [../CLAUDE.md](../CLAUDE.md) for the broader architecture, and
  [TESTING.md](TESTING.md) for how the test suite (which covers the VM core
  the CLI drives) is built and run.
