"""End-to-end edge-case tests of the RVV load/store instructions on the Kites simulator.

Each case is a small assembly program (written to this folder as <name>.s, so it can also be
opened in the GUI) that stores what it observed to `out` (0x10000000). The runner assembles and
runs it with kites-cli on the single-cycle VM and on both pipelined VMs with hazard detection, and
compares the bytes at `out` with the expected bytes, or, for the fault cases, checks that the run
stops with the expected error.

usage: python examples/vector_ldst_tests/run_tests.py [--cli PATH]
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
VMS = ["rvss", "rv5-h-nf", "rv5-h-f"]


def w(*vals):
    return b"".join(struct.pack("<i", v) for v in vals)


SRC_WORDS = ".word 1, 2, 3, 4, 5, 6, 7, 8"


def misaligned_expected():
    src = w(1, 2, 3, 4, 5, 6, 7, 8)
    loaded = src[2:18]                      # vle32 from src + 2
    out = bytearray(w(0, 0, 0, 0)) + bytearray(24)
    out[0:16] = loaded                      # aligned store to out
    out[21:37] = loaded                     # misaligned store to out + 21
    return bytes(out[:40])


# name -> (description, program, expected bytes at out, or ("error", text in the message))
CASES = {
    "vl_zero": ("vl = 0: loads and stores do nothing", """
    li       t1, 0
    vsetvli  t0, t1, e32, m1, ta, ma   # AVL 0 -> vl = 0
    la       a0, src
    la       a1, out
    vle32.v  v1, (a0)                  # no element is loaded
    vse32.v  v1, (a1)                  # no element is stored: out keeps 7, 7, 7, 7
    sw       t0, 16(a1)                # out[4] = vl = 0
.data
out: .word 7, 7, 7, 7, 9
src: """ + SRC_WORDS, w(7, 7, 7, 7, 0)),

    "tail_undisturbed": ("elements past vl are not loaded or stored", """
    la       a0, src
    la       a1, out
    vsetivli x0, 4, e32, m1, ta, ma
    vle32.v  v1, (a0)                  # v1 = 1, 2, 3, 4
    addi     a2, a0, 16
    vsetivli x0, 2, e32, m1, ta, ma
    vle32.v  v1, (a2)                  # vl = 2: v1 = 5, 6, 3, 4 (tail kept)
    vsetivli x0, 4, e32, m1, ta, ma
    vse32.v  v1, (a1)                  # out[0..3] = 5, 6, 3, 4
    addi     a3, a1, 16
    vsetivli x0, 3, e32, m1, ta, ma
    vse32.v  v1, (a3)                  # vl = 3: out[4..7] = 5, 6, 3, 9 (out[7] untouched)
.data
out: .word 0, 0, 0, 0, 9, 9, 9, 9
src: """ + SRC_WORDS, w(5, 6, 3, 4, 5, 6, 3, 9)),

    "masked": ("masked load and masked store (v0 = 0b1010)", """
    la       a0, src
    la       a1, out
    la       a2, mask
    vsetivli x0, 4, e32, m1, ta, ma
    vlm.v    v0, (a2)                  # v0 = 0b1010: elements 1 and 3 active
    vle32.v  v1, (a0)                  # v1 = 1, 2, 3, 4
    vse32.v  v1, (a1), v0.t            # out[0..3] = 9, 2, 9, 4
    vmv.v.i  v2, 5                     # v2 = 5, 5, 5, 5
    vle32.v  v2, (a0), v0.t            # v2 = 5, 2, 5, 4
    addi     a3, a1, 16
    vse32.v  v2, (a3)                  # out[4..7] = 5, 2, 5, 4
.data
out:  .word 9, 9, 9, 9, 0, 0, 0, 0
src:  """ + SRC_WORDS + """
mask: .byte 10""", w(9, 2, 9, 4, 5, 2, 5, 4)),

    "stride_negative_zero": ("negative stride (reverse) and zero stride (broadcast)", """
    la       a0, src
    la       a1, out
    vsetivli x0, 4, e32, m1, ta, ma
    addi     a2, a0, 28                # &src[7]
    li       t0, -4
    vlse32.v v1, (a2), t0              # v1 = 8, 7, 6, 5
    vse32.v  v1, (a1)
    vlse32.v v2, (a0), zero            # stride 0: v2 = 1, 1, 1, 1
    addi     a3, a1, 16
    vse32.v  v2, (a3)
.data
out: .zero 32
src: """ + SRC_WORDS, w(8, 7, 6, 5, 1, 1, 1, 1)),

    "indexed": ("indexed gather (vluxei32) and scatter (vsuxei32)", """
    la       a0, src
    la       a1, out
    la       a2, idx
    vsetivli x0, 4, e32, m1, ta, ma
    vle32.v  v3, (a2)                  # byte offsets 12, 0, 8, 4
    vluxei32.v v1, (a0), v3            # gather: v1 = 4, 1, 3, 2
    vse32.v  v1, (a1)
    vle32.v  v4, (a0)                  # v4 = 1, 2, 3, 4
    addi     a3, a1, 16
    vsuxei32.v v4, (a3), v3            # scatter: out[4..7] = 2, 4, 3, 1
.data
out: .zero 32
src: """ + SRC_WORDS + """
idx: .word 12, 0, 8, 4""", w(4, 1, 3, 2, 2, 4, 3, 1)),

    "segment": ("segment load/store: array of (x, y) pairs split and re-joined", """
    la       a0, pts
    la       a1, out
    vsetivli x0, 4, e32, m1, ta, ma
    vlseg2e32.v v4, (a0)               # v4 = x = 1, 2, 3, 4;  v5 = y = 10, 20, 30, 40
    vse32.v  v4, (a1)                  # out[0..3]  = x
    addi     a2, a1, 16
    vse32.v  v5, (a2)                  # out[4..7]  = y
    addi     a3, a1, 32
    vsseg2e32.v v4, (a3)               # out[8..15] = 1, 10, 2, 20, 3, 30, 4, 40
.data
out: .zero 64
pts: .word 1, 10, 2, 20, 3, 30, 4, 40""",
        w(1, 2, 3, 4, 10, 20, 30, 40, 1, 10, 2, 20, 3, 30, 4, 40)),

    "byte_elements": ("8-bit elements, and EEW (8) different from SEW (32)", """
    la       a0, bytes
    la       a1, out
    vsetivli x0, 8, e8, m1, ta, ma
    vle8.v   v1, (a0)                  # 8 bytes
    vse8.v   v1, (a1)                  # out[0..7] = 0x11 .. 0x88
    vsetivli x0, 4, e32, m1, ta, ma    # SEW = 32, but vle8 uses EEW = 8 (EMUL = 1/4)
    addi     a2, a0, 4
    vle8.v   v2, (a2)                  # 4 bytes: 0x55 0x66 0x77 0x88
    addi     a3, a1, 8
    vse8.v   v2, (a3)                  # out[8..11]
.data
out:   .zero 16
bytes: .byte 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88""",
        bytes([0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x55, 0x66, 0x77, 0x88, 0, 0, 0, 0])),

    "misaligned": ("misaligned base addresses for a load and a store", """
    la       a0, src
    la       a1, out
    vsetivli x0, 4, e32, m1, ta, ma
    addi     a2, a0, 2
    vle32.v  v1, (a2)                  # load from src + 2 (not 4-byte aligned)
    vse32.v  v1, (a1)                  # aligned store
    addi     a3, a1, 21
    vse32.v  v1, (a3)                  # store to out + 21 (not aligned)
.data
out: .zero 40
src: """ + SRC_WORDS, misaligned_expected()),

    "whole_register": ("whole-register load/store ignore vl (vl = 0 here)", """
    la       a0, src
    la       a1, out
    li       t1, 0
    vsetvli  t0, t1, e32, m1, ta, ma   # vl = 0
    vl1re32.v v1, (a0)                 # still moves a whole register (16 bytes)
    vs1r.v   v1, (a1)                  # out[0..3] = 1, 2, 3, 4
.data
out: .word 9, 9, 9, 9, 9
src: """ + SRC_WORDS, w(1, 2, 3, 4, 9)),

    "mask_bytes": ("vlm.v / vsm.v move ceil(vl / 8) bytes (vl = 10 -> 2 bytes)", """
    la       a0, mbytes
    la       a1, out
    vsetivli x0, 10, e8, m1, ta, ma
    vlm.v    v0, (a0)                  # loads 2 bytes
    vsm.v    v0, (a1)                  # stores 2 bytes: out = A5 03 77 77
.data
out:    .byte 0x77, 0x77, 0x77, 0x77
mbytes: .byte 0xA5, 0x03, 0xFF, 0xFF""", bytes([0xA5, 0x03, 0x77, 0x77])),

    "register_group_m8": ("LMUL = 8: one load/store moves 32 words through v8-v15", """
    la       a0, src
    la       a1, out
    li       t1, 32
    vsetvli  t0, t1, e32, m8, ta, ma   # vl = 32
    vle32.v  v8, (a0)
    vse32.v  v8, (a1)
.data
out: .zero 128
src: .word """ + ", ".join(str(100 + i) for i in range(32)), w(*range(100, 132))),

    "scalar_vector_ordering": ("scalar store then vector load, vector store then scalar load", """
    la       a0, src
    la       a1, out
    li       t0, 99
    sw       t0, 0(a0)                 # scalar store to src[0] ...
    vsetivli x0, 4, e32, m1, ta, ma
    vle32.v  v1, (a0)                  # ... must be seen by the next vector load: 99, 2, 3, 4
    vse32.v  v1, (a1)                  # vector store ...
    lw       t1, 0(a1)                 # ... must be seen by the next scalar load
    sw       t1, 16(a1)                # out[4] = 99
.data
out: .zero 20
src: """ + SRC_WORDS, w(99, 2, 3, 4, 99)),

    "fault_only_first": ("vle8ff.v: a fault after element 0 trims vl instead of trapping", """
    la       a1, out
    vsetivli x0, 8, e8, m1, ta, ma
    vmv.v.i  v1, 9                     # v1 = 9, 9, ..., 9
    li       a0, -4                    # 2^64 - 4: elements 0..2 valid, element 3 out of range
    vle8ff.v v1, (a0)                  # loads 3 bytes (0, 0, 0) and sets vl = 3
    vse8.v   v1, (a1)                  # stores only vl = 3 bytes: out = 00 00 00 77 77 ...
.data
out: .byte 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77""",
        bytes([0, 0, 0, 0x77, 0x77, 0x77, 0x77, 0x77])),

    "load_fault": ("an ordinary load that faults stops the run with an access fault", """
    vsetivli x0, 8, e8, m1, ta, ma
    li       a0, -4
    vle8.v   v1, (a0)                  # element 3 is out of range -> load access fault
.data
out: .word 0""", ("error", "Load access fault")),

    "illegal_group": ("misaligned register group is an illegal instruction", """
    la       a0, out
    vsetivli x0, 4, e32, m2, ta, ma    # LMUL = 2: groups must start at an even register
    vle32.v  v1, (a0)                  # v1 is not aligned -> illegal
.data
out: .word 0""", ("error", "Illegal")),
}


def find_cli():
    for build in ("mingw-test", "mingw-gui"):
        p = os.path.join(REPO, "build", build, "kites-cli.exe")
        if os.path.exists(p):
            return p
    return shutil.which("kites-cli")


def run_case(cli, env, name, vm):
    desc, prog, expected = CASES[name]
    asm = os.path.join(HERE, f"{name}.s")
    work = tempfile.mkdtemp(prefix="kites_ldst_")
    try:
        nbytes = 8 if isinstance(expected, tuple) else len(expected)
        rows = max(1, (nbytes + 7) // 8)
        p = subprocess.run([cli, asm, "--vm", vm, "--mem-dump", "0x10000000", str(rows)],
                           cwd=work, capture_output=True, text=True, timeout=300, env=env)
        if isinstance(expected, tuple):
            ok = p.returncode == 2 and expected[1].lower() in (p.stderr + p.stdout).lower()
            msg = next((l for l in p.stderr.splitlines() if "Run failed" in l), p.stderr.strip())
            return ok, msg
        dump = json.load(open(os.path.join(work, "vm_state", "memory_dump.json")))
        out = b"".join(int(dump[k], 16).to_bytes(8, "little")
                       for k in sorted(dump, key=lambda a: int(a, 16)))
        ok = p.returncode == 0 and out[:nbytes] == expected
        return ok, "" if ok else f"got {out[:nbytes].hex()} expected {expected.hex()}"
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default=find_cli())
    args = ap.parse_args()
    env = dict(os.environ)
    if os.name == "nt" and os.path.isdir(r"C:\msys64\ucrt64\bin"):
        env["PATH"] = r"C:\msys64\ucrt64\bin" + os.pathsep + env.get("PATH", "")
    for name, (desc, prog, expected) in CASES.items():
        with open(os.path.join(HERE, f"{name}.s"), "w", newline="\n") as f:
            f.write(f"# RVV load/store edge case: {desc}\n"
                    f"# Run: kites-cli examples/vector_ldst_tests/{name}.s --mem-dump 0x10000000 8\n"
                    ".text" + prog.rstrip() + "\n")
    results, failed = [], 0
    for name, (desc, _, _) in CASES.items():
        row = {"case": name, "description": desc}
        for vm in VMS:
            ok, msg = run_case(os.path.abspath(args.cli), env, name, vm)
            row[vm] = ok
            failed += not ok
            print(f"  {'ok' if ok else 'FAIL':4} {vm:9} {name:24} {msg}")
        results.append(row)
    json.dump(results, open(os.path.join(HERE, "results.json"), "w"), indent=1)
    total = len(CASES) * len(VMS)
    print(f"\n{total - failed} / {total} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
