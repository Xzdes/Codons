#!/usr/bin/env python3
"""Verify the portable WebAssembly backend against the native interpreter.

Runs a curated set of pure-compute .dna programs, emits a .wasm for each via
`codons emit-wasm`, executes the wasm with Node, and compares the result with
`codons interpret`.

Skips cleanly (exit 0) when the toolchain is unavailable: Node is looked up
as $NODE / PATH, clang as $CODONS_CLANG / PATH (the same lookup emit-wasm
performs).  Toolchain paths are never hardcoded.
"""
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUN_JS = ROOT / "tests" / "wasm" / "run.js"
WORK = Path.cwd() / "work"
WORK.mkdir(exist_ok=True)

NODE = os.environ.get("NODE") or shutil.which("node")
CLANG = os.environ.get("CODONS_CLANG") or shutil.which("clang")

# (program path relative to ROOT, args, expected result)
CASES = [
    ("tests/llm_eval/reference/add-two.dna", [5, 7], 12),
    ("tests/llm_eval/reference/add-two.dna", [-3, 4], 1),
    ("tests/llm_eval/reference/abs-value.dna", [-42], 42),
    ("tests/llm_eval/reference/abs-value.dna", [7], 7),
    ("tests/llm_eval/reference/factorial.dna", [5], 120),
    ("tests/llm_eval/reference/factorial.dna", [1], 1),
    ("tests/llm_eval/reference/fib.dna", [10], 55),
    ("tests/llm_eval/reference/fib.dna", [0], 0),
    ("tests/llm_eval/reference/clamp.dna", [1, 5, 10], 5),
    ("tests/llm_eval/reference/min-of-three.dna", [1, 2, 3], 1),
    # Literal second operands (0.17): shifts and division must not confuse
    # a literal with a register index (the original backend did exactly that).
    ("tests/wasm/shift-lit.dna", [5], 80),
    ("tests/wasm/shift-lit.dna", [-4], 2147483584),
    ("tests/wasm/divmod.dna", [-7, 3], -21),
    ("tests/wasm/divmod.dna", [7, -3], -19),
]

# Runtime faults in native Codons become wasm traps: both runs must FAIL.
FAIL_CASES = [
    ("tests/wasm/divmod.dna", [1, 0]),             # division by zero
    ("tests/wasm/divmod.dna", [-2147483648, -1]),  # division overflow
]


def run_native(codons, dna, args):
    proc = subprocess.run([str(codons), "interpret", str(dna), *[str(a) for a in args]],
                        capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"native interpret failed: {proc.stderr.strip()}")
    try:
        return int(proc.stdout.strip().splitlines()[-1])
    except Exception as exc:
        raise RuntimeError(f"native output: {proc.stdout!r}") from exc


def run_wasm(codons, dna, args):
    wasm = WORK / "out.wasm"
    proc = subprocess.run([str(codons), "emit-wasm", str(dna), str(wasm)],
                          capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"emit-wasm failed: {proc.stderr.strip()}")
    proc = subprocess.run([NODE, str(RUN_JS), str(wasm)] + [str(a) for a in args],
                          capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"wasm run failed: {proc.stderr.strip()}")
    try:
        return int(proc.stdout.strip().splitlines()[-1])
    except Exception as exc:
        raise RuntimeError(f"wasm output: {proc.stdout!r}") from exc


def fails_native(codons, dna, args):
    proc = subprocess.run([str(codons), "interpret", str(dna), *[str(a) for a in args]],
                          capture_output=True, text=True, encoding="utf-8")
    return proc.returncode != 0


def fails_wasm(codons, dna, args):
    wasm = WORK / "out_fail.wasm"
    proc = subprocess.run([str(codons), "emit-wasm", str(dna), str(wasm)],
                          capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        return None  # emission itself failed
    proc = subprocess.run([NODE, str(RUN_JS), str(wasm)] + [str(a) for a in args],
                          capture_output=True, text=True, encoding="utf-8")
    return proc.returncode != 0


def wasm_toolchain_ok():
    """The found clang must actually support the wasm32 target (Apple clang may not)."""
    probe_c = WORK / "probe_wasm.c"
    probe_c.write_text("int run(int x) { return x; }\n", encoding="utf-8")
    proc = subprocess.run([CLANG, "--target=wasm32-unknown-unknown", "-nostdlib",
                           "-Wl,--no-entry", "-Wl,--export=run", "-O0",
                           "-o", str(WORK / "probe.wasm"), str(probe_c)],
                          capture_output=True)
    return proc.returncode == 0


def main():
    codons = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build" / "codons.exe"
    if not NODE:
        print("SKIP wasm backend: node not found (set NODE or add node to PATH)")
        return 0
    if not CLANG:
        print("SKIP wasm backend: clang not found (set CODONS_CLANG or add clang to PATH)")
        return 0
    if not wasm_toolchain_ok():
        print("SKIP wasm backend: clang cannot target wasm32-unknown-unknown")
        return 0
    failures = 0
    for case in CASES:
        name, args, expected = case
        dna = ROOT / name
        if not dna.exists():
            print(f"FAIL {name}: source not found")
            failures += 1
            continue
        try:
            native = run_native(codons, dna, args)
            wasm = run_wasm(codons, dna, args)
        except Exception as exc:
            print(f"FAIL {name} {args}: {exc}")
            failures += 1
            continue
        if native != wasm or native != expected:
            print(f"FAIL {name} {args}: expected {expected}, native {native}, wasm {wasm}")
            failures += 1
        else:
            print(f"PASS {name} {args} -> {native}")
    for name, args in FAIL_CASES:
        dna = ROOT / name
        native_fails = fails_native(codons, dna, args)
        wasm_fails = fails_wasm(codons, dna, args)
        if native_fails and wasm_fails:
            print(f"PASS {name} {args} -> both backends fail (native fault / wasm trap)")
        else:
            print(f"FAIL {name} {args}: native_fails={native_fails} wasm_fails={wasm_fails}")
            failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
