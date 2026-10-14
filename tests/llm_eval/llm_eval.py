"""LLM evaluation harness: score candidate .dna solutions against tasks.jsonl.

Usage: python tests/llm_eval/llm_eval.py <codons.exe> --candidates <dir>
Candidates are <task-id>.dna files written by any model. For every task the
harness runs `check --json`, then every case via `run --json` (list tasks use
--input/--output temp files). The report names each failure class so prompt
and language changes can be measured against the same yardstick.
Exit code 0 only when every task passes.
"""
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path

EXE = Path(sys.argv.pop(1)).resolve()
WORK = Path.cwd() / "work" / "llm_eval"
WORK.mkdir(parents=True, exist_ok=True)

ERROR_CLASSES = [
    ("field-count", re.compile(r"exactly 3 fields")),
    ("comma-space", re.compile(r"comma")),
    ("uninitialized", re.compile(r"uninitialized")),
    ("undefined-name", re.compile(r"undefined")),
    ("arity", re.compile(r"expects \d+ arguments|at most four")),
    ("declaration-order", re.compile(r"must precede|duplicate local")),
    ("literal-rule", re.compile(r"literal")),
]


def classify(message):
    for name, pattern in ERROR_CLASSES:
        if pattern.search(message):
            return name
    return "other"


def call(*args):
    return subprocess.run([str(EXE), *map(str, args)], cwd=WORK,
                          capture_output=True, text=True, encoding="utf-8", timeout=15)


def run_case(candidate, case, index):
    cmd = [candidate, *[str(a) for a in case.get("args", [])], "--json"]
    if "input" in case:
        in_file = WORK / f"in{index}.txt"
        in_file.write_text("".join(f"{v}\n" for v in case["input"]), encoding="utf-8")
        cmd += ["--input", in_file]
    if "expected_output" in case:
        out_file = WORK / f"out{index}.txt"
        if out_file.exists():
            out_file.unlink()
        cmd += ["--output", out_file]
    proc = call("run", *cmd)
    try:
        data = json.loads(proc.stdout)
    except ValueError:
        return f"run: unparseable output {proc.stdout[:120]!r}"
    if not data.get("ok"):
        message = data["error"]["message"]
        return f"run-fault:{classify(message)}: {message.splitlines()[0][:100]}"
    if data.get("result") != case["expected"]:
        return f"wrong-answer: expected {case['expected']}, got {data.get('result')}"
    if "expected_output" in case:
        got = data.get("output_list")
        if got != case["expected_output"]:
            return f"wrong-output-list: expected {case['expected_output']}, got {got}"
    if "printed" in case and data.get("printed") != case["printed"]:
        return f"wrong-printed: expected {case['printed']!r}, got {data.get('printed')!r}"
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidates", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--only", help="comma-separated task ids to run")
    args = parser.parse_args()
    args.candidates = args.candidates.resolve()
    tasks_file = Path(__file__).with_name("tasks.jsonl")
    tasks = [json.loads(line) for line in
             tasks_file.read_text(encoding="utf-8").splitlines() if line.strip()]
    if args.only:
        wanted = set(args.only.split(","))
        tasks = [t for t in tasks if t["id"] in wanted]
    report = {"tasks": [], "summary": {"total": len(tasks), "pass": 0, "by_class": {}}}
    for task in tasks:
        candidate = args.candidates / f"{task['id']}.dna"
        entry = {"id": task["id"], "status": "pass", "detail": "",
                 "cases_total": len(task["cases"]), "cases_passed": 0}
        if not candidate.exists():
            entry["status"], entry["detail"] = "missing", "no candidate file"
        else:
            proc = call("check", candidate, "--json")
            try:
                data = json.loads(proc.stdout)
            except ValueError:
                data = {"ok": False, "error": {"stage": "compile", "message": proc.stderr}}
            if not data.get("ok"):
                message = data["error"]["message"]
                entry["status"] = f"compile:{classify(message)}"
                entry["detail"] = message.splitlines()[0][:160]
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
