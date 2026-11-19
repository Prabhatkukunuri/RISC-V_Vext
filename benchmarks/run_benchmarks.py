"""Runs the scalar and the vector version of each benchmark in benchmarks/programs/ on Kites
and compares them by the number of assembly instructions executed.

For every program:
  1. the input arrays are read from the program's own .data section and the expected result
     is computed here in Python (see REFERENCES);
  2. the program is run with `kites-cli --stats` on the single-cycle VM (rvss), which
     executes exactly one instruction per step and classifies every one of them;
  3. the bytes left in `out` (0x10000000) must equal the expected result, otherwise the
     program is reported as FAILED.
The metrics are defined in benchmarks/REPORT.md.

usage: python run_benchmarks.py [--cli PATH] [--out DIR] [--no-chart]
"""
import argparse
import csv
import json
import math
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
PROGRAMS = os.path.join(HERE, "programs")
OUT_ADDRESS = 0x10000000


def i32(values):
    return b"".join(struct.pack("<i", ((v + 2**31) % 2**32) - 2**31) for v in values)


def matmul(A, B, n=16):
    return [sum(A[i * n + k] * B[k * n + j] for k in range(n)) for i in range(n)
            for j in range(n)]


# name, title, problem size (elements), reference: input arrays -> expected bytes in `out`
BENCHMARKS = [
    ("vadd", "Vector addition", 256, lambda d: i32([p + q for p, q in zip(d["a"], d["b"])])),
    ("saxpy", "AXPY (3x + y)", 256, lambda d: i32([3 * p + q for p, q in zip(d["x"], d["y"])])),
    ("dot", "Dot product", 256, lambda d: i32([sum(p * q for p, q in zip(d["a"], d["b"]))])),
    ("max", "Array maximum", 256, lambda d: i32([max(d["a"])])),
    ("matmul", "Matrix multiplication 16x16", 256, lambda d: i32(matmul(d["A"], d["B"]))),
]


def read_arrays(path):
    """Returns {label: [values]} for every `.word` array in the program's .data section."""
    arrays, label = {}, None
    for line in open(path):
        line = line.split("#", 1)[0].strip()
        m = re.match(r"([A-Za-z_]\w*):\s*(.*)", line)
        if m:
            label, line = m.group(1), m.group(2)
        if line.startswith(".word") and label:
            arrays.setdefault(label, []).extend(int(v) for v in line[5:].split(","))
    return arrays


def find_cli():
    for build in ("mingw-test", "mingw-gui", "msvc-test", "build"):
        for name in ("kites-cli.exe", "kites-cli"):
            path = os.path.join(REPO, "build", build, name)
            if os.path.exists(path):
                return path
    return shutil.which("kites-cli")


def run_program(cli, asm, expected, env):
    """Runs one program; returns (stats_dump, problem or None)."""
    work = tempfile.mkdtemp(prefix="kites_bench_")
    try:
        rows = max(1, math.ceil(len(expected) / 8))
        cmd = [cli, asm, "--stats", "--mem-dump", f"0x{OUT_ADDRESS:x}", str(rows)]
        proc = subprocess.run(cmd, cwd=work, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL, timeout=600, env=env)
        state = os.path.join(work, "vm_state")

        def load(name):
            path = os.path.join(state, name)
            return json.load(open(path)) if os.path.exists(path) else None

        errors = (load("errors_dump.json") or {}).get("errors", ["no errors_dump.json"])
        stats = load("stats_dump.json")
        memory = load("memory_dump.json") or {}
        out = bytearray()
        for row in sorted(memory, key=lambda a: int(a, 16)):
            out += int(memory[row], 16).to_bytes(8, "little")
        if proc.returncode != 0 or errors or not stats:
            return stats, f"exit code {proc.returncode}, errors {errors}"
        if bytes(out[:len(expected)]) != expected:
            return stats, "wrong result in `out`"
        return stats, None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def metrics(stats, elements):
    """The comparison metrics of one run (definitions in REPORT.md)."""
    m = stats["instruction_mix"]
    mem = stats["memory_bytes"]
    vec = stats["vector"]
    total = stats["instructions_retired"]
    vector_data = m["vector_load"] + m["vector_store"] + m["vector_compute"]
    return {
        "instructions": total,
        "instructions_per_element": total / elements,
        "alu": m["scalar_alu"],
        "load_store": m["scalar_load"] + m["scalar_store"],
        "branch_jump": m["branch"] + m["jump"],
        "vector_config": m["vector_config"],
        "vector_load_store": m["vector_load"] + m["vector_store"],
        "vector_compute": m["vector_compute"],
        "memory_instructions": m["scalar_load"] + m["scalar_store"] + m["vector_load"]
        + m["vector_store"],
        "bytes_moved": sum(mem.values()),
        "branches": m["branch"],
        "elements_per_vector_instruction": (vec["elements"] / vector_data) if vector_data
        else 0.0,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cli", default=find_cli(), help="path to kites-cli")
    parser.add_argument("--out", default=os.path.join(HERE, "results"))
    parser.add_argument("--no-chart", action="store_true")
    args = parser.parse_args()
    if not args.cli or not os.path.exists(args.cli):
        sys.exit("kites-cli not found; build it (see README) or pass --cli")
    cli = os.path.abspath(args.cli)

    env = dict(os.environ)
    msys = r"C:\msys64\ucrt64\bin"  # Qt / MinGW runtime DLLs for a MinGW build on Windows
    if os.name == "nt" and os.path.isdir(msys):
        env["PATH"] = msys + os.pathsep + env.get("PATH", "")

    results, failures = [], []
    for name, title, elements, reference in BENCHMARKS:
        entry = {"name": name, "title": title, "elements": elements}
        for variant in ("scalar", "vector"):
            asm = os.path.join(PROGRAMS, f"{name}_{variant}.s")
            expected = reference(read_arrays(asm))
            stats, problem = run_program(cli, asm, expected, env)
            print(f"  {'ok' if not problem else 'FAILED':6} {name}_{variant}.s"
                  + (f"  {problem}" if problem else ""))
            if problem:
                failures.append(f"{name}_{variant}.s: {problem}")
            else:
                entry[variant] = metrics(stats, elements)
        if "scalar" in entry and "vector" in entry:
            s, v = entry["scalar"], entry["vector"]
            entry["speedup"] = s["instructions"] / v["instructions"]
            entry["memory_instruction_reduction"] = (s["memory_instructions"]
                                                     / v["memory_instructions"])
            entry["branch_reduction"] = s["branches"] / v["branches"]
            results.append(entry)

    os.makedirs(args.out, exist_ok=True)
    with open(os.path.join(args.out, "results.json"), "w") as f:
        json.dump({"benchmarks": results, "failures": failures}, f, indent=2)
    write_csv(results, os.path.join(args.out, "summary.csv"))
    write_markdown(results, failures, os.path.join(args.out, "RESULTS.md"))
    if not args.no_chart:
        try:
            import charts
            charts.instructions_chart(results, os.path.join(args.out, "speedup.png"))
        except ImportError as e:
            print(f"chart skipped ({e}); install matplotlib to get it")
    print(f"\nresults written to {args.out}")
    if failures:
        sys.exit("FAILURES:\n  " + "\n  ".join(failures))
    print("all programs produced the expected output")


def write_csv(results, path):
    keys = ["instructions", "instructions_per_element", "alu", "load_store", "branch_jump",
            "vector_config", "vector_load_store", "vector_compute", "memory_instructions",
            "bytes_moved", "branches", "elements_per_vector_instruction"]
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["benchmark", "version"] + keys + ["speedup"])
        for e in results:
            for v in ("scalar", "vector"):
                w.writerow([e["name"], v] + [round(e[v][k], 3) for k in keys]
                           + [round(e["speedup"], 3) if v == "vector" else ""])


def write_markdown(results, failures, path):
    out = ["# Benchmark results\n",
           "Generated by `benchmarks/run_benchmarks.py`; the metrics are defined in "
           "[`../REPORT.md`](../REPORT.md). Single-cycle VM, VLEN = 128 bits. Every program "
           "produced exactly the expected output.\n",
           "## Instructions executed and speedup\n",
           "| Benchmark | Scalar | Vector | Speedup | Scalar instr./element | "
           "Vector instr./element |",
           "|---|---:|---:|---:|---:|---:|"]
    for e in results:
        s, v = e["scalar"], e["vector"]
        out.append(f"| {e['title']} | {s['instructions']:,} | {v['instructions']:,} | "
                   f"**{e['speedup']:.1f}x** | {s['instructions_per_element']:.2f} | "
                   f"{v['instructions_per_element']:.2f} |")
    out += ["", "![Instructions executed](speedup.png)\n",
            "## Instruction mix\n",
            "| Benchmark | Version | ALU | Load/store | Branch/jump | vset | Vector load/store "
            "| Vector compute | Total |",
            "|---|---|---:|---:|---:|---:|---:|---:|---:|"]
    for e in results:
        for v in ("scalar", "vector"):
            b = e[v]
            out.append(f"| {e['title'] if v == 'scalar' else ''} | {v} | {b['alu']:,} | "
                       f"{b['load_store']:,} | {b['branch_jump']:,} | {b['vector_config']:,} | "
                       f"{b['vector_load_store']:,} | {b['vector_compute']:,} | "
                       f"{b['instructions']:,} |")
    out += ["", "## Memory instructions, branches and vector utilisation\n",
            "| Benchmark | Memory instr. (scalar / vector) | Reduction | Bytes moved (scalar / "
            "vector) | Branches (scalar / vector) | Elements per vector instr. |",
            "|---|---:|---:|---:|---:|---:|"]
    for e in results:
        s, v = e["scalar"], e["vector"]
        out.append(f"| {e['title']} | {s['memory_instructions']:,} / "
                   f"{v['memory_instructions']:,} | {e['memory_instruction_reduction']:.1f}x | "
                   f"{s['bytes_moved']:,} / {v['bytes_moved']:,} | {s['branches']:,} / "
                   f"{v['branches']:,} | {v['elements_per_vector_instruction']:.1f} |")
    if failures:
        out += ["", "## Failures\n"] + [f"* {f}" for f in failures]
    with open(path, "w", newline="\n") as f:
        f.write("\n".join(out) + "\n")


if __name__ == "__main__":
    main()
