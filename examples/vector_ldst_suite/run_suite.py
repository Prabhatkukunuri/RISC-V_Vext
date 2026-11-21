"""Template-wise test suite for the RVV 1.0 vector loads and stores on the Kites simulator.

The cases are grouped by instruction template:
    unit-stride, strided, indexed, segment, whole-register, mask, fault-only-first (and faults).

Every case is a generated assembly program (written to programs/<template>/<name>.s, so it can be
opened in the GUI) with the same structure:
  1. v8-v31 are filled with a known byte pattern, v0 with the mask, v16.. with the indices;
  2. vsetvli sets SEW, LMUL and the AVL of the case (tail and mask undisturbed: tu, mu);
  3. the instruction under test runs once;
  4. vl is read back without a CSR instruction (vmset.m + vcpop.m), and v8-v31, vl and the store
     area are written to memory.

The expected bytes come from a small reference model of the RVV 1.0 load/store rules written in
this file (independent of the simulator's C++ code): addresses for every template, register
groups (EMUL), segments, masking, tail elements, whole-register and mask lengths, unsigned
indices, address wrap-around, and fault-only-first trimming. Error cases expect the run to stop
with the given message.

Each case runs on the single-cycle VM and both pipelined VMs with hazard detection.

usage: py -3.12 examples/vector_ldst_suite/run_suite.py [--cli PATH]
Writes examples/vector_ldst_suite/results.json.
"""
import argparse
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field
from fractions import Fraction

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
VMS = ["rvss", "rv5-h-nf", "rv5-h-f"]
VLENB = 16                    # VLEN = 128
TOP = 2**64 - 2               # last valid byte address (memory size 0xFFFFFFFFFFFFFFFF)

# ---- data layout (offsets from 0x10000000) ----
OUT, OUT_SIZE = 0, 400        # v8..v31 (384 bytes), vl (4 bytes), padding
DST, DST_SIZE = 400, 256      # store target, initially 0x55
SRC, SRC_SIZE = 656, 512      # load source: byte k = (k * 37 + 11) mod 256
INIT, INIT_SIZE = 1168, 384   # initial contents of v8..v31: byte k = 0x80 + k mod 128
MASKD = 1552                  # 16 mask bytes
IDX = 1568                    # index values (up to 128 bytes)
BASE = 0x10000000
DUMP_BYTES = DST + DST_SIZE

SRC_BYTES = bytes((k * 37 + 11) % 256 for k in range(SRC_SIZE))
INIT_BYTES = bytes(0x80 + k % 128 for k in range(INIT_SIZE))

LMULS = {"mf8": Fraction(1, 8), "mf4": Fraction(1, 4), "mf2": Fraction(1, 2),
         "m1": Fraction(1), "m2": Fraction(2), "m4": Fraction(4), "m8": Fraction(8)}


@dataclass
class Case:
    template: str
    name: str
    desc: str
    instr: str
    sew: int = 32
    lmul: str = "m1"
    avl: int = 4
    masked: bool = False
    mask: int = 0
    base: tuple = ("src", 0)      # ("src"|"dst", offset) or ("abs", address)
    stride: int = 0
    index: list = field(default_factory=list)
    vd: int = 8
    vs2: int = 16
    expect: object = "ok"         # "ok" or (stage, text): "run" = processor error,
                                  # "asm" = assembler error
    invalid_vtype: bool = False   # run the instruction with vill set


# ---------------------------------------------------------------------------------------------
#  Reference model
# ---------------------------------------------------------------------------------------------
def parse(mnemonic):
    """-> dict(kind, store, eew, nf, ff, ordered)"""
    m = mnemonic
    r = dict(store=m.startswith("vs"), nf=1, ff=False, ordered=False)
    if m in ("vlm.v", "vsm.v", "vle1.v", "vse1.v"):
        return dict(r, kind="mask", eew=8)
    w = re.fullmatch(r"v([ls])(\d)r(?:e(\d+))?\.v", m)
    if w:
        return dict(r, kind="whole", nf=int(w.group(2)), eew=int(w.group(3) or 8))
    x = re.fullmatch(r"v[ls](u|o)xseg(\d)ei(\d+)\.v|v[ls](u|o)xei(\d+)\.v", m)
    if x:
        if x.group(1):
            return dict(r, kind="indexed", ordered=x.group(1) == "o", nf=int(x.group(2)),
                        eew=int(x.group(3)))
        return dict(r, kind="indexed", ordered=x.group(4) == "o", eew=int(x.group(5)))
    s = re.fullmatch(r"v[ls]sseg(\d)e(\d+)\.v|v[ls]se(\d+)\.v", m)
    if s:
        if s.group(1):
            return dict(r, kind="strided", nf=int(s.group(1)), eew=int(s.group(2)))
        return dict(r, kind="strided", eew=int(s.group(3)))
    u = re.fullmatch(r"v[ls]seg(\d)e(\d+)(ff)?\.v|v[ls]e(\d+)(ff)?\.v", m)
    if u:
        if u.group(1):
            return dict(r, kind="unit", nf=int(u.group(1)), eew=int(u.group(2)),
                        ff=bool(u.group(3)))
        return dict(r, kind="unit", eew=int(u.group(4)), ff=bool(u.group(5)))
    raise ValueError(m)


class Fault(Exception):
    pass


def model(c):
    """Expected (registers v8..v31, vl, dst bytes) after the case, per the RVV 1.0 spec."""
    regs = bytearray(32 * VLENB)
    regs[8 * VLENB:] = INIT_BYTES
    regs[0:VLENB] = c.mask.to_bytes(VLENB, "little")
    p = parse(c.instr)
    if p["kind"] == "indexed" and c.index:
        ib = p["eew"] // 8
        data = b"".join(v.to_bytes(ib, "little") for v in c.index)
        regs[c.vs2 * VLENB:c.vs2 * VLENB + len(data)] = data
    mem = {}  # address -> byte (writes); reads fall back to the initial data

    def initial(a):
        o = a - BASE
        if DST <= o < DST + DST_SIZE:
            return 0x55
        if SRC <= o < SRC + SRC_SIZE:
            return SRC_BYTES[o - SRC]
        return 0

    def rd(a, n):
        a %= 2**64
        if a + n - 1 > TOP:
            raise Fault
        return bytes(mem.get(a + k, initial(a + k)) for k in range(n))

    def wr(a, data):
        a %= 2**64
        if a + len(data) - 1 > TOP:
            raise Fault
        for k, b in enumerate(data):
            mem[a + k] = b

    if c.base[0] == "abs":
        base = c.base[1] % 2**64
    else:
        base = BASE + (SRC if c.base[0] == "src" else DST) + c.base[1]
    lmul = LMULS[c.lmul]
    vlmax = int(lmul * VLENB * 8 / c.sew)
    vl = min(c.avl, vlmax)
    if p["kind"] == "whole":
        n = p["nf"] * VLENB
        if p["store"]:
            wr(base, bytes(regs[c.vd * VLENB:c.vd * VLENB + n]))
        else:
            regs[c.vd * VLENB:c.vd * VLENB + n] = rd(base, n)
    elif p["kind"] == "mask":
        n = math.ceil(vl / 8)
        if p["store"]:
            wr(base, bytes(regs[c.vd * VLENB:c.vd * VLENB + n]))
        elif n:
            regs[c.vd * VLENB:c.vd * VLENB + n] = rd(base, n)
    else:
        indexed = p["kind"] == "indexed"
        db = (c.sew if indexed else p["eew"]) // 8          # data element bytes
        demul = lmul if indexed else Fraction(p["eew"], c.sew) * lmul
        per_field = max(1, int(demul))                       # registers per field
        for i in range(vl):
            if c.masked and not (c.mask >> i) & 1:
                continue                                     # masked off: undisturbed
            if indexed:
                ib = p["eew"] // 8
                o = c.vs2 * VLENB + i * ib
                off = int.from_bytes(regs[o:o + ib], "little")  # zero-extended
            fields = []
            try:
                for f in range(p["nf"]):
                    if p["kind"] == "unit":
                        a = base + (i * p["nf"] + f) * db
                    elif p["kind"] == "strided":
                        a = base + i * c.stride + f * db
                    else:
                        a = base + off + f * db
                    r = (c.vd + f * per_field) * VLENB + i * db
                    if p["store"]:
                        wr(a, bytes(regs[r:r + db]))
                    else:
                        fields.append((r, rd(a, db)))
            except Fault:
                if p["ff"] and i > 0:
                    vl = i                                   # fault-only-first: trim vl
                    break
                raise
            for r, data in fields:                           # whole segment, then commit
                regs[r:r + db] = data
    dst = bytes(mem.get(BASE + DST + k, 0x55) for k in range(DST_SIZE))
    return bytes(regs[8 * VLENB:]), vl, dst


# ---------------------------------------------------------------------------------------------
#  Programs
# ---------------------------------------------------------------------------------------------
def byte_lines(label, data):
    rows = [", ".join(str(b) for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    return f"{label}:\n" + "".join(f"    .byte {r}\n" for r in rows)


def program(c):
    p = parse(c.instr)
    L = [f"# Kites RVV load/store suite -- {c.template}: {c.name}",
         f"# {c.desc}",
         f"# Run: kites-cli examples/vector_ldst_suite/programs/{c.template}/{c.name}.s "
         f"--mem-dump 0x10000000 {DUMP_BYTES // 8}",
         ".text",
         "    # 1. known starting state: v8-v31 = pattern, v0 = mask",
         "    li       t0, 128",
         "    vsetvli  x0, t0, e8, m8, ta, ma",
         "    la       t1, init",
         "    vle8.v   v8, (t1)",
         "    addi     t1, t1, 128",
         "    vle8.v   v16, (t1)",
         "    addi     t1, t1, 128",
         "    vle8.v   v24, (t1)",
         "    vsetivli x0, 16, e8, m1, ta, ma",
         "    la       t1, maskdata",
         "    vle8.v   v0, (t1)"]
    if p["kind"] == "indexed" and c.index:
        L += [f"    li       t0, {len(c.index)}              # index vector",
              f"    vsetvli  x0, t0, e{p['eew']}, m8, ta, ma",
              "    la       t1, idx",
              f"    vle{p['eew']}.v  v{c.vs2}, (t1)"]
    L += ["    # 2. configuration of the case"]
    if c.invalid_vtype:
        L += ["    li       t0, 4",
              "    vsetvli  t2, t0, e64, mf8, tu, mu  # SEW 64 with LMUL 1/8: vill = 1"]
    else:
        L += [f"    li       t0, {c.avl}",
              f"    vsetvli  t2, t0, e{c.sew}, {c.lmul}, tu, mu"]
    if c.base[0] == "abs":
        L += [f"    li       a0, {c.base[1]}"]
    else:
        L += [f"    la       a0, {c.base[0]}",
              f"    addi     a0, a0, {c.base[1]}"]
    if p["kind"] == "strided":
        L += [f"    li       a1, {c.stride}"]
    ops = f"v{c.vd}, (a0)"
    if p["kind"] == "strided":
        ops += ", a1"
    if p["kind"] == "indexed":
        ops += f", v{c.vs2}"
    if c.masked:
        ops += ", v0.t"
    L += ["    # 3. the instruction under test", f"    {c.instr:<12} {ops}"]
    L += ["    # 4. results: vl (vcpop of an all-ones mask), v8-v31"]
    if not c.invalid_vtype:
        L += ["    vmset.m  v1",
              "    vcpop.m  t3, v1",
              "    la       t1, out",
              "    sw       t3, 384(t1)"]
    L += ["    li       t0, 128",
          "    vsetvli  x0, t0, e8, m8, ta, ma",
          "    la       t1, out",
          "    vse8.v   v8, (t1)",
          "    addi     t1, t1, 128",
          "    vse8.v   v16, (t1)",
          "    addi     t1, t1, 128",
          "    vse8.v   v24, (t1)",
          ".data",
          f"out:  .zero {OUT_SIZE}"]
    L.append(byte_lines("dst", bytes([0x55]) * DST_SIZE).rstrip())
    L.append(byte_lines("src", SRC_BYTES).rstrip())
    L.append(byte_lines("init", INIT_BYTES).rstrip())
    L.append(byte_lines("maskdata", c.mask.to_bytes(16, "little")).rstrip())
    if p["kind"] == "indexed" and c.index:
        ib = p["eew"] // 8
        data = b"".join(v.to_bytes(ib, "little") for v in c.index)
        L.append(byte_lines("idx", data).rstrip())
    return "\n".join(L) + "\n"


# ---------------------------------------------------------------------------------------------
#  Cases
# ---------------------------------------------------------------------------------------------
# Error cases: (stage, text). "run": the processor stops the run ("Run failed: <text>");
# "asm": the assembler rejects the program (message in errors_dump.json).
ALT = 0x5555  # elements 0, 2, 4, ...


def C(template, name, desc, instr, **kw):
    return Case(template, name, desc, instr, **kw)


CASES = [
    # ---- unit-stride ---------------------------------------------------------------------------
    C("unit", "e8", "vle8.v, SEW 8, vl 16", "vle8.v", sew=8, avl=16),
    C("unit", "e16", "vle16.v, SEW 16, vl 8", "vle16.v", sew=16, avl=8),
    C("unit", "e32", "vle32.v, SEW 32, vl 4", "vle32.v"),
    C("unit", "e64", "vle64.v, SEW 64, vl 2", "vle64.v", sew=64, avl=2),
    C("unit", "lmul8_full", "vle32.v with LMUL 8 and vl = VLMAX = 32 (v8-v15)", "vle32.v",
      lmul="m8", avl=32),
    C("unit", "lmul4_tail", "vle32.v with LMUL 4 and vl 13: tail across registers kept", "vle32.v",
      lmul="m4", avl=13),
    C("unit", "eew8_sew32", "vle8.v at SEW 32: EEW < SEW, EMUL 1/4", "vle8.v", avl=4),
    C("unit", "eew64_sew16", "vle64.v at SEW 16, LMUL 1: EMUL 4 (v8-v11)", "vle64.v", sew=16,
      avl=8),
    C("unit", "fractional_lmul", "vle16.v at SEW 32, LMUL 1/2: EMUL 1/4", "vle16.v", lmul="mf2",
      avl=2),
    C("unit", "vl0_load", "vle32.v with vl = 0 changes nothing", "vle32.v", avl=0),
    C("unit", "vl1_load", "vle32.v with vl = 1", "vle32.v", avl=1),
    C("unit", "masked_load", "vle32.v, LMUL 2, mask 0101...", "vle32.v", lmul="m2", avl=8,
      masked=True, mask=ALT),
    C("unit", "mask_all_zero", "masked vle32.v with an all-zero mask changes nothing",
      "vle32.v", avl=4, masked=True, mask=0),
    C("unit", "misaligned_load", "vle32.v from src + 1 (not 4-byte aligned)", "vle32.v",
      base=("src", 1)),
    C("unit", "store_e8", "vse8.v, vl 16", "vse8.v", sew=8, avl=16, base=("dst", 0)),
    C("unit", "store_e16_tail", "vse16.v, vl 7: memory after element 6 untouched", "vse16.v",
      sew=16, avl=7, base=("dst", 0)),
    C("unit", "store_e64_lmul2", "vse64.v, LMUL 2, vl 3", "vse64.v", sew=64, lmul="m2", avl=3,
      base=("dst", 0)),
    C("unit", "masked_store", "vse32.v, mask 0101...", "vse32.v", avl=4, masked=True, mask=ALT,
      base=("dst", 0)),
    C("unit", "vl0_store", "vse32.v with vl = 0 writes nothing", "vse32.v", avl=0,
      base=("dst", 0)),
    C("unit", "misaligned_store", "vse64.v to dst + 3", "vse64.v", sew=64, avl=2,
      base=("dst", 3)),
    C("unit", "illegal_emul", "vle64.v at SEW 8, LMUL 2: EMUL 16 > 8", "vle64.v", sew=8,
      lmul="m2", expect=("run", "EMUL = EEW/SEW * LMUL is out of range")),
    C("unit", "illegal_alignment", "vle32.v into v9 with LMUL 2 (odd register)", "vle32.v",
      lmul="m2", vd=9, expect=("run", "vd v9 is not aligned to a group of 2")),
    C("unit", "illegal_masked_v0", "masked vle32.v whose destination is v0", "vle32.v",
      masked=True, mask=ALT, vd=0, expect=("asm", "Destination overlaps the mask register")),
    C("unit", "illegal_vill", "vle32.v after a vsetvli that set vill", "vle32.v",
      invalid_vtype=True, expect=("run", "vtype.vill is set")),

    # ---- strided -------------------------------------------------------------------------------
    C("strided", "stride_eq_size", "vlse32.v, stride 4 (same as unit-stride)", "vlse32.v",
      stride=4),
    C("strided", "stride_12", "vlse32.v, stride 12 (every third word)", "vlse32.v", stride=12),
    C("strided", "negative", "vlse16.v, stride -2 from the end: reversed", "vlse16.v", sew=16,
      avl=8, stride=-2, base=("src", 14)),
    C("strided", "zero_load", "vlse32.v, stride 0: one element broadcast", "vlse32.v", stride=0,
      base=("src", 8)),
    C("strided", "e64_lmul2", "vlse64.v, SEW 64, LMUL 2, stride 24", "vlse64.v", sew=64,
      lmul="m2", avl=3, stride=24),
    C("strided", "odd_stride", "vlse32.v, stride 6: misaligned elements", "vlse32.v", stride=6),
    C("strided", "large_stride", "vlse32.v, stride 128", "vlse32.v", stride=128),
    C("strided", "eew8_sew32", "vlse8.v at SEW 32, stride 3", "vlse8.v", stride=3),
    C("strided", "masked_load", "vlse32.v, stride 8, mask 0101...", "vlse32.v", stride=8,
      masked=True, mask=ALT),
    C("strided", "vl0", "vlse32.v with vl = 0", "vlse32.v", avl=0, stride=8),
    C("strided", "store", "vsse32.v, stride 8: gaps untouched", "vsse32.v", stride=8,
      base=("dst", 0)),
    C("strided", "store_negative", "vsse16.v, stride -4", "vsse16.v", sew=16, avl=6, stride=-4,
      base=("dst", 40)),
    C("strided", "store_zero", "vsse32.v, stride 0: the last element wins", "vsse32.v",
      stride=0, base=("dst", 0)),
    C("strided", "store_masked", "vsse32.v, stride 12, mask 0101...", "vsse32.v", stride=12,
      masked=True, mask=ALT, base=("dst", 0)),

    # ---- indexed -------------------------------------------------------------------------------
    C("indexed", "unordered_ei32", "vluxei32.v gather with byte offsets 12, 0, 8, 4",
      "vluxei32.v", index=[12, 0, 8, 4]),
    C("indexed", "ordered_ei32", "vloxei32.v gather with byte offsets 12, 0, 8, 4", "vloxei32.v",
      index=[12, 0, 8, 4]),
    C("indexed", "ei8_sew32", "vluxei8.v at SEW 32 (index EMUL 1/4), unsigned offset 252",
      "vluxei8.v", index=[252, 0, 100, 4]),
    C("indexed", "ei16_sew32", "vluxei16.v at SEW 32", "vluxei16.v", index=[300, 2, 64, 1]),
    C("indexed", "ei64_sew32", "vluxei64.v at SEW 32 (index EMUL 2)", "vluxei64.v",
      index=[8, 16, 0, 24]),
    C("indexed", "ei8_sew64", "vluxei8.v at SEW 64, LMUL 2", "vluxei8.v", sew=64, lmul="m2",
      index=[40, 0, 8, 200]),
    C("indexed", "repeated", "vluxei32.v with repeated offsets", "vluxei32.v",
      index=[20, 20, 20, 0]),
    C("indexed", "wraparound", "vluxei64.v with offset 2^64 - 4: base - 4", "vluxei64.v",
      sew=64, avl=2, index=[2**64 - 4, 0], base=("src", 16)),
    C("indexed", "masked_load", "vluxei32.v, mask 0101...", "vluxei32.v", index=[12, 0, 8, 4],
      masked=True, mask=ALT),
    C("indexed", "vl0", "vluxei32.v with vl = 0", "vluxei32.v", avl=0, index=[12, 0, 8, 4]),
    C("indexed", "vd_equals_vs2", "vluxei32.v with vd = vs2 (same EEW, legal)", "vluxei32.v",
      index=[12, 0, 8, 4], vd=16),
    C("indexed", "scatter_unordered", "vsuxei32.v scatter", "vsuxei32.v", index=[12, 0, 8, 4],
      base=("dst", 0)),
    C("indexed", "scatter_ordered_dup", "vsoxei32.v with duplicate offsets: last element wins",
      "vsoxei32.v", index=[4, 4, 0, 4], base=("dst", 0)),
    C("indexed", "scatter_ei8_sew16", "vsoxei8.v at SEW 16", "vsoxei8.v", sew=16, avl=6,
      index=[10, 0, 2, 30, 6, 8], base=("dst", 0)),
    C("indexed", "scatter_masked", "vsuxei32.v, mask 0101...", "vsuxei32.v",
      index=[12, 0, 8, 4], masked=True, mask=ALT, base=("dst", 0)),
    C("indexed", "illegal_overlap", "vluxei8.v at SEW 32 with vd = vs2 (different EEW)",
      "vluxei8.v", index=[0, 4, 8, 12], vd=16,
      expect=("run", "overlaps the index register group")),

    # ---- segment -------------------------------------------------------------------------------
    C("segment", "seg2_e32", "vlseg2e32.v: (x, y) pairs into v8 and v9", "vlseg2e32.v"),
    C("segment", "seg3_e8", "vlseg3e8.v: RGB bytes into v8, v9, v10", "vlseg3e8.v", sew=8,
      avl=8),
    C("segment", "seg8_e8", "vlseg8e8.v: 8 fields (the maximum)", "vlseg8e8.v", sew=8, avl=4),
    C("segment", "seg4_lmul2", "vlseg4e16.v at LMUL 2 (NFIELDS x EMUL = 8)", "vlseg4e16.v",
      sew=16, lmul="m2", avl=10),
    C("segment", "seg2_eew8_sew32", "vlseg2e8.v at SEW 32 (EMUL 1/4)", "vlseg2e8.v", avl=4),
    C("segment", "seg2_tail_masked", "vlseg2e32.v, vl 3, mask 0101...", "vlseg2e32.v", avl=3,
      masked=True, mask=ALT),
    C("segment", "seg_vl0", "vlseg3e32.v with vl = 0", "vlseg3e32.v", avl=0),
    C("segment", "store_seg2", "vsseg2e32.v: v8, v9 interleaved", "vsseg2e32.v",
      base=("dst", 0)),
    C("segment", "store_seg3_masked", "vsseg3e8.v, mask 0101...", "vsseg3e8.v", sew=8, avl=6,
      masked=True, mask=ALT, base=("dst", 0)),
    C("segment", "strided_seg2", "vlsseg2e16.v, stride 12 (first 2 fields of 12-byte records)",
      "vlsseg2e16.v", sew=16, avl=5, stride=12),
    C("segment", "strided_store_seg2", "vssseg2e32.v, stride 16", "vssseg2e32.v", stride=16,
      base=("dst", 0)),
    C("segment", "indexed_seg2", "vluxseg2ei8.v at SEW 32", "vluxseg2ei8.v",
      index=[24, 0, 16, 8]),
    C("segment", "indexed_store_seg3", "vsoxseg3ei16.v at SEW 8", "vsoxseg3ei16.v", sew=8,
      avl=4, index=[9, 0, 6, 3], base=("dst", 0)),
    C("segment", "illegal_nf_emul", "vlseg3e32.v at LMUL 4: NFIELDS x EMUL = 12 > 8",
      "vlseg3e32.v", lmul="m4", expect=("run", "NFIELDS * EMUL exceeds 8 registers")),
    C("segment", "illegal_past_v31", "vlseg4e32.v into v30: fields run past v31", "vlseg4e32.v",
      vd=30, expect=("run", "segment register groups run past v31")),

    # ---- whole register ------------------------------------------------------------------------
    C("whole", "vl1re8_vl0", "vl1re8.v ignores vl = 0", "vl1re8.v", avl=0),
    C("whole", "vl2re16", "vl2re16.v into v8-v9", "vl2re16.v"),
    C("whole", "vl4re32", "vl4re32.v into v8-v11", "vl4re32.v"),
    C("whole", "vl8re64", "vl8re64.v into v8-v15", "vl8re64.v"),
    C("whole", "alias_vl1r", "vl1r.v (alias of vl1re8.v)", "vl1r.v"),
    C("whole", "ignores_vtype", "vl1re32.v with SEW 64, LMUL 8, vl 1, mask 0", "vl1re32.v",
      sew=64, lmul="m8", avl=1),
    C("whole", "with_vill", "vl1re32.v after a vsetvli that set vill", "vl1re32.v",
      invalid_vtype=True),
    C("whole", "misaligned", "vl2re32.v from src + 2", "vl2re32.v", base=("src", 2)),
    C("whole", "store_vs1r", "vs1r.v", "vs1r.v", base=("dst", 0)),
    C("whole", "store_vs2r", "vs2r.v", "vs2r.v", base=("dst", 0)),
    C("whole", "store_vs4r", "vs4r.v", "vs4r.v", base=("dst", 0)),
    C("whole", "store_vs8r_vl0", "vs8r.v with vl = 0 (128 bytes)", "vs8r.v", avl=0,
      base=("dst", 0)),
    C("whole", "illegal_alignment", "vl2re32.v into v9", "vl2re32.v", vd=9, expect=("asm", "Misaligned register group")),

    # ---- mask ----------------------------------------------------------------------------------
    C("mask", "vlm_vl10", "vlm.v, vl 10: 2 bytes", "vlm.v", sew=8, avl=10),
    C("mask", "vlm_vl16", "vlm.v, vl 16: 2 bytes", "vlm.v", sew=8, avl=16),
    C("mask", "vlm_vl17", "vlm.v, vl 17: 3 bytes", "vlm.v", sew=8, avl=17),
    C("mask", "vlm_vl0", "vlm.v, vl 0: nothing", "vlm.v", sew=8, avl=0),
    C("mask", "vlm_sew32_m8", "vlm.v at SEW 32, LMUL 8, vl 32: 4 bytes", "vlm.v", lmul="m8",
      avl=32),
    C("mask", "alias_vle1", "vle1.v (alias of vlm.v), vl 12", "vle1.v", sew=8, avl=12),
    C("mask", "vsm_vl10", "vsm.v, vl 10: 2 bytes", "vsm.v", sew=8, avl=10, base=("dst", 0)),
    C("mask", "vsm_vl1", "vsm.v, vl 1: 1 byte", "vsm.v", sew=8, avl=1, base=("dst", 0)),
    C("mask", "vsm_vl0", "vsm.v, vl 0: nothing", "vsm.v", sew=8, avl=0, base=("dst", 0)),

    # ---- fault-only-first and access faults -----------------------------------------------------
    C("fof", "no_fault", "vle32ff.v without a fault: same as vle32.v, vl unchanged",
      "vle32ff.v"),
    C("fof", "trim_e8", "vle8ff.v at 2^64 - 4, vl 8: element 3 faults, vl = 3", "vle8ff.v",
      sew=8, avl=8, base=("abs", -4)),
    C("fof", "trim_e32", "vle32ff.v at 2^64 - 12: element 2 faults, vl = 2", "vle32ff.v",
      base=("abs", -12)),
    C("fof", "trim_segment", "vlseg2e8ff.v at 2^64 - 6: field 1 of element 2 faults, vl = 2",
      "vlseg2e8ff.v", sew=8, avl=4, base=("abs", -6)),
    C("fof", "element0_faults", "vle8ff.v at 2^64 - 1: element 0 faults, trap", "vle8ff.v",
      sew=8, avl=8, base=("abs", -1), expect=("run", "Load access fault in 0x03050407 at element 0")),
    C("fof", "load_fault", "vle32.v at 2^64 - 8: access fault", "vle32.v", base=("abs", -8),
      expect=("run", "Load access fault in 0x02056407 at element 1")),
    C("fof", "store_fault", "vse32.v at 2^64 - 8: access fault", "vse32.v", base=("abs", -8),
      expect=("run", "Store access fault in 0x02056427 at element 1")),
    C("fof", "strided_fault", "vlse32.v at 2^64 - 12, stride 8: element 1 faults", "vlse32.v",
      stride=8, base=("abs", -12), expect=("run", "Load access fault in 0x0ab56407 at element 1")),
    C("fof", "indexed_fault", "vluxei64.v with an offset reaching 2^64 - 1", "vluxei64.v",
      sew=64, avl=2, index=[0, 2**64 - 1 - (BASE + SRC)],
      expect=("run", "Load access fault in 0x07057407 at element 1")),
]

TEMPLATES = ["unit", "strided", "indexed", "segment", "whole", "mask", "fof"]


# ---------------------------------------------------------------------------------------------
#  Runner
# ---------------------------------------------------------------------------------------------
def find_cli():
    for build in ("mingw-test", "mingw-gui"):
        p = os.path.join(REPO, "build", build, "kites-cli.exe")
        if os.path.exists(p):
            return p
    return shutil.which("kites-cli")


def run_case(cli, env, c, asm, vm):
    work = tempfile.mkdtemp(prefix="kites_suite_")
    try:
        p = subprocess.run([cli, asm, "--vm", vm, "--mem-dump", "0x10000000",
                            str(DUMP_BYTES // 8)], cwd=work, capture_output=True, text=True,
                           timeout=300, env=env)
        text = p.stdout + p.stderr
        run_errors = [l.split("Run failed:", 1)[1].strip() for l in p.stderr.splitlines()
                      if l.startswith("Run failed:") and l.strip() != "Run failed."]
        asm_errors = []
        if "Assembly failed" in text:
            err = os.path.join(work, "vm_state", "errors_dump.json")
            if os.path.exists(err):
                asm_errors = [e.get("message", "") for e in json.load(open(err))["errors"]]
        if c.expect != "ok":
            stage, want = c.expect
            got = run_errors if stage == "run" else asm_errors
            ok = p.returncode != 0 and any(want in m for m in got)
            msg = f"[{'run' if run_errors else 'asm' if asm_errors else '?'}] " +                 "; ".join(run_errors or asm_errors or [text.strip()[-150:]])
            return ok, msg
        dump = json.load(open(os.path.join(work, "vm_state", "memory_dump.json")))
        out = b"".join(int(dump[k], 16).to_bytes(8, "little")
                       for k in sorted(dump, key=lambda a: int(a, 16)))
        regs, vl, dst = model(c)
        got_regs, got_vl = out[:384], int.from_bytes(out[384:388], "little")
        got_dst = out[DST:DST + DST_SIZE]
        problems = []
        if p.returncode != 0:
            problems.append(f"exit {p.returncode}: {text.strip()[-200:]}")
        if got_regs != regs:
            k = next(i for i in range(384) if got_regs[i] != regs[i])
            problems.append(f"v{8 + k // 16} byte {k % 16}: got {got_regs[k]:#04x} "
                            f"expected {regs[k]:#04x}")
        if not c.invalid_vtype and got_vl != vl:
            problems.append(f"vl {got_vl} expected {vl}")
        if got_dst != dst:
            k = next(i for i in range(DST_SIZE) if got_dst[i] != dst[i])
            problems.append(f"dst+{k}: got {got_dst[k]:#04x} expected {dst[k]:#04x}")
        return not problems, "; ".join(problems)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", default=find_cli())
    args = ap.parse_args()
    cli = os.path.abspath(args.cli)
    env = dict(os.environ)
    if os.name == "nt" and os.path.isdir(r"C:\msys64\ucrt64\bin"):
        env["PATH"] = r"C:\msys64\ucrt64\bin" + os.pathsep + env.get("PATH", "")
    assert len({(c.template, c.name) for c in CASES}) == len(CASES)
    results, failed = [], 0
    for t in TEMPLATES:
        cases = [c for c in CASES if c.template == t]
        print(f"{t} ({len(cases)} cases)")
        os.makedirs(os.path.join(HERE, "programs", t), exist_ok=True)
        for c in cases:
            asm = os.path.join(HERE, "programs", t, f"{c.name}.s")
            with open(asm, "w", newline="\n") as f:
                f.write(program(c))
            row = dict(template=t, case=c.name, description=c.desc, instruction=c.instr,
                       expect="ok" if c.expect == "ok" else list(c.expect))
            for vm in VMS:
                ok, msg = run_case(cli, env, c, asm, vm)
                row[vm] = ok
                row.setdefault("messages", {})[vm] = msg
                failed += not ok
                if not ok or vm == "rvss":
                    print(f"  {'ok' if ok else 'FAIL':4} {vm:9} {c.name:22} {c.desc[:60]:60} "
                          f"{msg[:120]}")
            results.append(row)
    # Negative controls: a mutated program checked against the original expectation must fail.
    controls = [("unit", "masked_load", ", v0.t", "", "mask removed from a masked load"),
                ("strided", "store_zero", "li       a1, 0", "li       a1, 4",
                 "stride 0 replaced by 4"),
                ("segment", "seg3_e8", "vlseg3e8.v", "vlseg2e8.v", "3 fields replaced by 2"),
                ("whole", "vl4re32", "vl4re32.v", "vl2re32.v", "4 registers replaced by 2"),
                ("mask", "vlm_vl17", "vlm.v", "vle8.v", "vlm.v replaced by vle8.v"),
                ("fof", "trim_e8", "vle8ff.v", "vle8.v", "fault-only-first replaced by vle8.v"),
                ("indexed", "unordered_ei32", "vluxei32.v", "vluxei8.v",
                 "index EEW 32 replaced by 8")]
    neg = []
    for t, name, old, new, note in controls:
        c = next(x for x in CASES if x.template == t and x.name == name)
        text = program(c)
        if old.endswith(".v"):  # a mnemonic: replace it in the instruction line only
            old, new = f"    {old:<12} ", f"    {new:<12} "
        assert text.count(old) == 1, (name, old)
        work = tempfile.mkdtemp(prefix="kites_neg_")
        asm = os.path.join(work, "mutant.s")
        open(asm, "w", newline="\n").write(text.replace(old, new))
        ok, msg = run_case(cli, env, c, asm, "rvss")
        shutil.rmtree(work, ignore_errors=True)
        neg.append(dict(template=t, case=name, mutation=note, caught=not ok, detail=msg))
        print(f"  {'caught' if not ok else 'MISSED'} negative control {t}/{name}: {note}: {msg[:90]}")
        failed += ok
    total = len(CASES) * len(VMS)
    json.dump(dict(passed=total - sum(not r[vm] for r in results for vm in VMS), total=total,
                   cases=results, negative_controls=neg),
              open(os.path.join(HERE, "results.json"), "w"), indent=1)
    print(f"\n{len(CASES)} cases x {len(VMS)} VMs: {total - failed} / {total} passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
