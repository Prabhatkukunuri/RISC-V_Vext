"""Checks the MLP inference example and compares the scalar and vector programs.

  1. builds and runs mlp.c (the reference): hidden values, scores and predicted digit;
  2. runs mlp_scalar.s and mlp_vector.s on the simulator (kites-cli --stats);
  3. checks that both programs wrote exactly the same results as the C program;
  4. prints the comparison of the two programs.

usage: py -3.12 benchmarks/mlp/verify_mlp.py
"""
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
CLI = os.path.join(REPO, "build", "mingw-test", "kites-cli.exe")
MACS = 64 * 32 + 32 * 10  # multiply-accumulates in one inference

env = dict(os.environ)
if os.name == "nt" and os.path.isdir(r"C:\msys64\ucrt64\bin"):
    env["PATH"] = r"C:\msys64\ucrt64\bin" + os.pathsep + env["PATH"]
work = tempfile.mkdtemp(prefix="kites_mlp_")

# 1. the C program
exe = os.path.join(work, "mlp.exe")
gcc = shutil.which("gcc", path=env["PATH"]) or "gcc"
subprocess.run([gcc, "-O2", "-Wall", "-Wextra", "-Werror", "-fwrapv", "-o", exe,
                os.path.join(HERE, "mlp.c")], check=True, env=env)
lines = subprocess.run([exe], capture_output=True, text=True, check=True, env=env).stdout.split("\n")
hidden = [int(v) for v in lines[0].split()[1:]]
logits = [int(v) for v in lines[1].split()[1:]]
cls = int(lines[2].split()[2])
print(f"mlp.c: {lines[2]}")
expected = struct.pack(f"<{1 + 10 + 32}i", cls, *logits, *hidden)  # cls, logits, hidden

# 2./3. the two assembly programs
stats, ok = {}, True
for variant in ("scalar", "vector"):
    p = subprocess.run([CLI, os.path.join(HERE, f"mlp_{variant}.s"), "--stats",
                        "--mem-dump", "0x10000000", "22"], cwd=work, capture_output=True,
                       text=True, env=env)
    state = os.path.join(work, "vm_state")
    mem = json.load(open(os.path.join(state, "memory_dump.json")))
    got = b"".join(int(mem[k], 16).to_bytes(8, "little") for k in sorted(mem, key=lambda a: int(a, 16)))
    same = p.returncode == 0 and got[:len(expected)] == expected
    ok &= same
    stats[variant] = json.load(open(os.path.join(state, "stats_dump.json")))
    print(f"mlp_{variant}.s: predicted digit {struct.unpack('<i', got[:4])[0]}, "
          f"{'same results as mlp.c' if same else 'DIFFERENT from mlp.c'}")
shutil.rmtree(work, ignore_errors=True)

# 4. comparison
def row(name, s, v, ratio=True):
    r = f"{s / v:8.2f}x" if ratio and v else ""
    print(f"  {name:28} {s:>10,} {v:>10,} {r}")

s, v = stats["scalar"], stats["vector"]
mix_s, mix_v = s["instruction_mix"], v["instruction_mix"]
mem = lambda m: m["scalar_load"] + m["scalar_store"] + m["vector_load"] + m["vector_store"]
print(f"\n  {'':28} {'scalar':>10} {'vector':>10}  speedup")
row("instructions", s["instructions_retired"], v["instructions_retired"])
print(f"  {'instructions per MAC':28} {s['instructions_retired'] / MACS:>10.2f} "
      f"{v['instructions_retired'] / MACS:>10.2f}")
row("memory instructions", mem(mix_s), mem(mix_v))
row("bytes moved", sum(s["memory_bytes"].values()), sum(v["memory_bytes"].values()))
row("branches", mix_s["branch"], mix_v["branch"])
row("vector instructions", 0, mix_v["vector_config"] + mix_v["vector_load"] + mix_v["vector_store"]
    + mix_v["vector_compute"], ratio=False)
sys.exit(0 if ok else 1)
