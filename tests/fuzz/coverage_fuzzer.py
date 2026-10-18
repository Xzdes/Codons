#!/usr/bin/env python3
"""Coverage-guided mutational fuzzer for the Codons PARSER and ANALYZER.

This is intentionally a separate module from tests/fuzz_diff.py (which compares
the JIT vs interpreter BACKENDS). This fuzzer drives the front-end only: it runs
the compiled ``codons`` binary in ``check`` mode, which calls ``bio_load()`` —
i.e. ``bio_parse()`` followed by ``bio_analyze()``, with no JIT and no execution.

Coverage signal
---------------
The binary is built ONCE into a separate, gcov-instrumented build directory
(``build-fuzz/``) with ``--coverage``. Every ``codons check <input>`` run updates
the ``.gcda`` counter files (they accumulate across runs). After each mutation we
invoke ``gcov -f -n`` on the front-end translation units (parser.c, analyze.c,
core.c) and parse the per-function "Lines executed: X% of N" summaries to compute
how many executable lines and functions have ever been executed. A mutation is
"interesting" (added to the corpus) when it increases the covered-line or
covered-function count.

Note: child stdout/stderr are redirected to temp FILES (never captured through a
pipe) because piped capture is blocked in the sandbox; files are read back after
the process exits.

Usage
-----
    python tests/fuzz/coverage_fuzzer.py --iterations 600
    python tests/fuzz/coverage_fuzzer.py --iterations 2000 --seed 42

Requires a gcov-instrumented build first (see docs/fuzzing.md).
"""

import argparse
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build-fuzz"
CORPUS_DIR = ROOT / "tests" / "fuzz" / "corpus"
INTERESTING_DIR = BUILD / "coverage_interesting"   # inputs that grew coverage
CRASH_DIR = BUILD / "coverage_crashes"             # crashed/hung inputs
REPORT_PATH = BUILD / "coverage_fuzzer_report.txt"

# Front-end translation units whose coverage we track. core.c holds bio_load()
# (the parse+analyze wrapper), parser.c the parser, analyze.c the analyzer.
TRACKED_SOURCES = ["src/parser.c", "src/analyze.c", "src/core.c"]

GCOV = shutil.which("gcov") or "gcov"
BINARY = None
for cand in ("codons.exe", "codons"):
    p = BUILD / cand
    if p.exists():
        BINARY = p
        break

_LINE_RE = re.compile(r"Lines executed:\s*([0-9.]+)% of (\d+)")
_BRANCH_RE = re.compile(r"Branches executed:\s*([0-9.]+)% of (\d+)")
_CALL_RE = re.compile(r"Calls executed:\s*([0-9.]+)% of (\d+)")
_FUNC_RE = re.compile(r"^Function '(.*)'$")


def run_hidden(args, timeout=None):
    """Run a command, redirecting stdout/stderr to temp files (no pipe capture).

    Returns (returncode, stdout_text, stderr_text). A timeout is surfaced as a
    TimeoutExpired exception by the caller.
    """
    fd_out, out_path = tempfile.mkstemp(prefix="cfuzz_out_", suffix=".txt")
    fd_err, err_path = tempfile.mkstemp(prefix="cfuzz_err_", suffix=".txt")
    os.close(fd_out)
    os.close(fd_err)
    try:
        with open(out_path, "wb") as fo, open(err_path, "wb") as fe:
            proc = subprocess.run(list(args), stdout=fo, stderr=fe,
                                  timeout=timeout)
        with open(out_path, "r", encoding="utf-8", errors="replace") as fo:
            out_text = fo.read()
        with open(err_path, "r", encoding="utf-8", errors="replace") as fe:
            err_text = fe.read()
        return proc.returncode, out_text, err_text
    finally:
        try:
            os.remove(out_path)
        except OSError:
            pass
        try:
            os.remove(err_path)
        except OSError:
            pass


def gcov_metrics_for_source(src_rel):
    """Run gcov for one translation unit and return (lines_total, lines_cov,
    funcs_total, funcs_cov)."""
    src_abs = (ROOT / src_rel).resolve()
    gcno = (BUILD / "CMakeFiles" / "bio.dir" / "src" / (Path(src_rel).name + ".gcno")).resolve()
    if not gcno.exists():
        return 0, 0, 0, 0
    rc, out, _err = run_hidden([GCOV, "-f", "-n", "-o", str(gcno), str(src_abs)])
    if rc != 0:
        return 0, 0, 0, 0

    lines_total = 0
    lines_cov = 0
    funcs_total = 0
    funcs_cov = 0
    current = {}
    for line in out.splitlines():
        mfunc = _FUNC_RE.match(line)
        if mfunc:
            if current:
                funcs_total += 1
                if current.get("cov", 0) > 0:
                    funcs_cov += 1
                lines_total += current.get("total", 0)
                lines_cov += current.get("cov", 0)
            current = {"name": mfunc.group(1), "total": 0, "cov": 0}
            continue
        mline = _LINE_RE.search(line)
        if mline:
            pct = float(mline.group(1))
            total = int(mline.group(2))
            current["total"] = total
            current["cov"] = int(round(total * pct / 100.0))
    if current:
        funcs_total += 1
        if current.get("cov", 0) > 0:
            funcs_cov += 1
        lines_total += current.get("total", 0)
        lines_cov += current.get("cov", 0)
    return lines_total, lines_cov, funcs_total, funcs_cov


def measure():
    """Aggregate gcov coverage across the tracked translation units.

    Returns a dict keyed by source relative path plus aggregated totals.
    """
    out = {}
    agg = {"lines": 0, "lines_cov": 0, "funcs": 0, "funcs_cov": 0}
    for src in TRACKED_SOURCES:
        lt, lc, ft, fc = gcov_metrics_for_source(src)
        out[src] = {"lines": lt, "lines_cov": lc, "funcs": ft, "funcs_cov": fc}
        agg["lines"] += lt
        agg["lines_cov"] += lc
        agg["funcs"] += ft
        agg["funcs_cov"] += fc
    return out, agg


def is_new_better(best_l, best_f, agg):
    """True when the aggregate covered lines/functions grew."""
    if agg["funcs_cov"] > best_f:
        return True
    if agg["funcs_cov"] == best_f and agg["lines_cov"] > best_l:
        return True
    return False


def run_check(data: bytes, timeout=2.0):
    """Write `data` to a temp .dna file and run `codons check <file>`.

    Returns (returncode, stderr_text). A return code outside {0, 1} indicates a
    crash (signal / abort / access violation); 1 is a normal parse/analyze
    rejection; 0 is a valid program.
    """
    fd, path = tempfile.mkstemp(prefix="cfuzz_input_", suffix=".dna")
    os.close(fd)
    try:
        with open(path, "wb") as f:
            f.write(data)
        rc, _out, err = run_hidden([str(BINARY), "check", path], timeout=timeout)
        return rc, err
    finally:
        try:
            os.remove(path)
        except OSError:
            pass


def mutate(rng, db, corpus):
    """Apply a random byte-level mutation to `db` into a new bytes object."""
    data = bytearray(db)
    strategy = rng.randrange(7)
    n = len(data)
    if strategy == 0 and n > 0:
        # flip a bit
        i = rng.randrange(n)
        data[i] ^= 1 << rng.randrange(8)
    elif strategy == 1 and n > 0:
        # random byte substitution
        i = rng.randrange(n)
        data[i] = rng.randrange(256)
    elif strategy == 2 and n > 0:
        # insert random bytes
        i = rng.randrange(n + 1)
        k = rng.randrange(1, 16)
        data[i:i] = bytes(rng.randrange(256) for _ in range(k))
    elif strategy == 3 and n > 0:
        # delete a span
        i = rng.randrange(n)
        k = rng.randrange(1, min(16, n - i) + 1)
        del data[i:i + k]
    elif strategy == 4 and n > 0:
        # duplicate a span
        i = rng.randrange(n)
        k = rng.randrange(1, min(16, n - i) + 1)
        data[i:i] = data[i:i + k]
    elif strategy == 5 and n > 0:
        # overwrite a span with random printable ASCII (DNA-ish tokens)
        i = rng.randrange(n)
        k = rng.randrange(1, min(16, n - i) + 1)
        data[i:i + k] = bytes(rng.choice(b"0123456789abcdefghijklmnopqrstuvwxyz "
                                         b"[](){},:;#.+*-/\n\t")
                              for _ in range(k))
    else:
        # splice two corpus entries
        other = rng.choice(corpus)
        if n > 1 and len(other) > 1:
            cut = rng.randrange(1, n)
            data = bytearray(data[:cut] + other[rng.randrange(len(other)):])
    return bytes(data)


def reset_gcda():
    for gcda in BUILD.rglob("*.gcda"):
        try:
            gcda.unlink()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--iterations", type=int, default=600)
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--no-reset", action="store_true",
                    help="do not clear .gcda before starting")
    args = ap.parse_args()

    if BINARY is None:
        print("FATAL: no gcov-instrumented binary in build-fuzz/. "
              "Build it first (see docs/fuzzing.md).", file=sys.stderr)
        sys.exit(2)
    if not GCOV:
        print("FATAL: gcov not found on PATH.", file=sys.stderr)
        sys.exit(2)

    rng = random.Random(args.seed)
    if not args.no_reset:
        reset_gcda()

    # ---- seed corpus -----------------------------------------------------
    corpus = []
    if CORPUS_DIR.is_dir():
        for p in sorted(CORPUS_DIR.iterdir()):
            if p.is_file():
                corpus.append(p.read_bytes())
    if not corpus:
        corpus = [b"gene main: () {}\n"]
    rng.shuffle(corpus)

    INTERESTING_DIR.mkdir(parents=True, exist_ok=True)
    CRASH_DIR.mkdir(parents=True, exist_ok=True)

    lines = []
    log = lines.append

    log("=== codons coverage fuzzer (parser/analyzer) ===")
    log("binary    : %s" % BINARY)
    log("gcov      : %s" % GCOV)
    log("iterations: %d" % args.iterations)
    log("seed      : %s" % args.seed)
    log("tracked   : %s" % ", ".join(TRACKED_SOURCES))
    log("")

    # ---- baseline: run every seed through check once, then measure ------
    log("Seeding: %d corpus files" % len(corpus))
    for data in corpus:
        rc, err = run_check(data)
        if rc not in (0, 1):
            log("  [pre-fuzz crash rc=%s on seed of %d bytes, saved]" % (rc, len(data)))
            _save(CRASH_DIR, "seed_crash", data)

    _, before = measure()
    log("BASELINE coverage BEFORE fuzzing:")
    log("  lines  : %d / %d (%.1f%%)" % (
        before["lines_cov"], before["lines"],
        100.0 * before["lines_cov"] / max(1, before["lines"])))
    log("  funcs  : %d / %d (%.1f%%)" % (
        before["funcs_cov"], before["funcs"],
        100.0 * before["funcs_cov"] / max(1, before["funcs"])))
    log("")

    best_l = before["lines_cov"]
    best_f = before["funcs_cov"]
    interesting = 0
    crashes = []
    hangs = []

    t0 = time.time()
    for it in range(1, args.iterations + 1):
        parent = rng.choice(corpus)
        data = mutate(rng, parent, corpus)
        if len(data) == 0 or len(data) > 4 * 1024 * 1024:
            continue

        try:
            rc, err = run_check(data, timeout=2.0)
        except subprocess.TimeoutExpired:
            hangs.append(data)
            _save(CRASH_DIR, "hang", data)
            continue

        if rc not in (0, 1):
            entry = dict(rc=rc, err=err.strip()[:400], data=data)
            crashes.append(entry)
            _save(CRASH_DIR, "crash_rc%d" % rc, data)

        _, agg = measure()
        if is_new_better(best_l, best_f, agg):
            best_l = agg["lines_cov"]
            best_f = agg["funcs_cov"]
            interesting += 1
            corpus.append(data)
            if len(corpus) > 4096:
                corpus = corpus[:4096]
            _save(INTERESTING_DIR, "cov_%04d" % it, data)

        if it % 50 == 0:
            dt = time.time() - t0
            log("[iter %4d] lines %d/%d  funcs %d/%d  corpus=%d  interesting=%d  %.1f it/s" % (
                it, best_l, agg["lines"], best_f, agg["funcs"],
                len(corpus), interesting, it / max(dt, 1e-9)))

    _, after = measure()
    log("")
    log("=== RESULT ===")
    log("iterations run      : %d" % args.iterations)
    log("interesting inputs  : %d (kept in corpus)" % interesting)
    log("crashes             : %d" % len(crashes))
    log("hangs               : %d" % len(hangs))
    log("")
    log("BEFORE: lines %d/%d (%.1f%%)  funcs %d/%d (%.1f%%)" % (
        before["lines_cov"], before["lines"],
        100.0 * before["lines_cov"] / max(1, before["lines"]),
        before["funcs_cov"], before["funcs"],
        100.0 * before["funcs_cov"] / max(1, before["funcs"])))
    log("AFTER : lines %d/%d (%.1f%%)  funcs %d/%d (%.1f%%)" % (
        after["lines_cov"], after["lines"],
        100.0 * after["lines_cov"] / max(1, after["lines"]),
        after["funcs_cov"], after["funcs"],
        100.0 * after["funcs_cov"] / max(1, after["funcs"])))
    log("new lines           : +%d" % (after["lines_cov"] - before["lines_cov"]))
    log("new functions       : +%d" % (after["funcs_cov"] - before["funcs_cov"]))
    log("")
    log("per-source:")
    for src, m in measure()[0].items():
        log("  %-16s lines %d/%d  funcs %d/%d" % (
            src, m["lines_cov"], m["lines"], m["funcs_cov"], m["funcs"]))
    if crashes:
        log("")
        log("CRASH DETAILS:")
        for c in crashes[:20]:
            log("  rc=%s stderr=%r" % (c["rc"], c["err"]))
    if hangs:
        log("  hangs: %d" % len(hangs))

    text = "\n".join(lines)
    print(text)
    REPORT_PATH.write_text(text + "\n", encoding="utf-8")


def _save(dest_dir, prefix, data):
    dest_dir.mkdir(parents=True, exist_ok=True)
    path = dest_dir / ("%s_%d.dna" % (prefix, len(list(dest_dir.iterdir()))))
    path.write_bytes(data)


if __name__ == "__main__":
    main()