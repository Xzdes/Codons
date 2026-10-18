#!/usr/bin/env python3
"""Drives the Python reference oracle against the native interpreter.

For every .dna in the reference corpus, runs both the oracle and the native
interpreter on the same set of cases and reports mismatches.  Exit code 0 only
when every checked program produces identical results.
"""
import argparse
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
ORACLE = ROOT / "tests" / "reference" / "oracle.py"
REF_DIR = ROOT / "tests" / "llm_eval" / "reference"

CASES = {
    "add-two.dna": [[5, 7], [-3, 4]],
    "abs-value.dna": [[-42], [7]],
    "clamp.dna": [[1, 5, 10], [10, 5, 1]],
    "factorial.dna": [[5], [1], [0]],
    "fib.dna": [[10], [0]],
    "min-of-three.dna": [[1, 2, 3], [3, 2, 1]],
}


def native_result(codons: Path, dna: Path, args: list) -> int:
    proc = subprocess.run([str(codons), "interpret", str(dna), *[str(a) for a in args]],
                          capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"native failed: {proc.stderr}")
    return int(proc.stdout.strip().splitlines()[-1])


def oracle_result(codons: Path, dna: Path, args: list) -> int:
    proc = subprocess.run([sys.executable, str(ORACLE), str(dna), *[str(a) for a in args],
                           "--codons", str(codons)],
                          capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"oracle failed: {proc.stderr}")
    return int(proc.stdout.strip().splitlines()[-1])


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--codons", type=Path, default=ROOT / "build" / "codons.exe")
    args = parser.parse_args()
    failures = 0
    for name, cases in CASES.items():
        dna = REF_DIR / name
        for case in cases:
            try:
                n = native_result(args.codons, dna, case)
                o = oracle_result(args.codons, dna, case)
                if n != o:
                    print(f"MISMATCH {name} {case}: native={n} oracle={o}")
                    failures += 1
                else:
                    print(f"PASS {name} {case} -> {n}")
            except Exception as exc:
                print(f"ERROR {name} {case}: {exc}")
                failures += 1
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
