"""Python arm of the LLM eval: score candidate .py solutions against the same
tasks.jsonl used for Codons, so the comparison is task-for-task.

Candidate contract (stated verbatim in the generation prompt):
- scalar arguments arrive as sys.argv[1:] (decimal i32 values);
- an input list arrives on stdin, one integer per line;
- the result integer is printed to STDERR;
- an output list is printed to stdout, one element per line;
- text to print goes to stdout exactly.

Usage: python tests/llm_eval/py_eval.py --candidates <dir> [--report out.json]
Exit code 0 only when every task passes.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

PY = sys.executable
ERROR_CLASSES = [
    ("syntax", re.compile(r"SyntaxError|IndentationError")),
    ("name-error", re.compile(r"NameError")),
    ("type-error", re.compile(r"TypeError")),
    ("index-error", re.compile(r"IndexError")),
    ("value-error", re.compile(r"ValueError")),
    ("zero-div", re.compile(r"ZeroDivisionError")),
    ("recursion", re.compile(r"RecursionError")),
]


def classify(message):
    for name, pattern in ERROR_CLASSES:
        if pattern.search(message):
            return name
    return "other"


def run_case(candidate, case, index):
    stdin_text = ""
    if "input" in case:
        stdin_text = "".join(f"{v}\n" for v in case["input"])
    cmd = [PY, str(candidate), *[str(a) for a in case.get("args", [])]]
    try:
        proc = subprocess.run(cmd, input=stdin_text, capture_output=True,
                              text=True, encoding="utf-8", timeout=8)
    except subprocess.TimeoutExpired:
        return "timeout: exceeded 8s wall-clock (Codons uses a deterministic 1M-step budget)"
    if proc.returncode != 0:
        last = [ln for ln in proc.stderr.strip().splitlines() if ln.strip()]
        message = last[-1] if last else f"exit {proc.returncode}"
        return f"crash:{classify(message)}: {message[:100]}"
    err_lines = [ln for ln in proc.stderr.strip().splitlines() if ln.strip()]
    if not err_lines:
        return "wrong-answer: nothing printed to stderr (result channel)"
    try:
        result = int(err_lines[-1].strip())
    except ValueError:
        return f"wrong-answer: stderr last line is not an int: {err_lines[-1][:60]!r}"
    if result != case["expected"]:
        return f"wrong-answer: expected {case['expected']}, got {result}"
    if "expected_output" in case:
        try:
            got = [int(ln) for ln in proc.stdout.strip().splitlines() if ln.strip()]
        except ValueError:
            return "wrong-output-list: stdout lines are not all ints"
        if got != case["expected_output"]:
            return f"wrong-output-list: expected {case['expected_output']}, got {got}"
    elif "printed" in case:
        if proc.stdout != case["printed"]:
            return f"wrong-printed: expected {case['printed']!r}, got {proc.stdout!r}"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidates", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--only", help="comma-separated task ids to run")
    args = parser.parse_args()
    tasks_file = Path(__file__).with_name("tasks.jsonl")
    tasks = [json.loads(line) for line in
             tasks_file.read_text(encoding="utf-8").splitlines() if line.strip()]
    if args.only:
        wanted = set(args.only.split(","))
        tasks = [t for t in tasks if t["id"] in wanted]
    report = {"lang": "python", "tasks": [],
              "summary": {"total": len(tasks), "pass": 0, "by_class": {}}}
    for task in tasks:
        candidate = args.candidates / f"{task['id']}.py"
        entry = {"id": task["id"], "status": "pass", "detail": "",
                 "cases_total": len(task["cases"]), "cases_passed": 0}
        if not candidate.exists():
            entry["status"], entry["detail"] = "missing", "no candidate file"
        else:
            for index, case in enumerate(task["cases"]):
                problem = run_case(candidate, case, index)
                if problem:
                    entry["status"] = "run:" + problem.split(":", 1)[0]
                    entry["detail"] = problem[:160]
                    break
                entry["cases_passed"] += 1
        report["tasks"].append(entry)
        if entry["status"] == "pass":
            report["summary"]["pass"] += 1
        else:
            cls = entry["status"]
            report["summary"]["by_class"][cls] = report["summary"]["by_class"].get(cls, 0) + 1
        print(f"{entry['id']:<18} {entry['status']:<24} "
              f"{entry['cases_passed']}/{entry['cases_total']} {entry['detail']}")
    s = report["summary"]
    print(f"\n{s['pass']}/{s['total']} tasks pass; failures by class: {s['by_class']}")
    if args.report:
        args.report.write_text(json.dumps(report, indent=2), encoding="utf-8")
    return 0 if s["pass"] == s["total"] else 1


if __name__ == "__main__":
    sys.exit(main())
