"""Exercise the shipped executable as a user or AI client would. Standard library only."""
import json
import csv
import io
import os
import random
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

EXE = Path(sys.argv.pop(1)).resolve()
EXAMPLES = Path(__file__).resolve().parents[1] / "examples"
WORK = Path.cwd() / "work"
WORK.mkdir(exist_ok=True)


def i32(value):
    return (value + 2**31) % 2**32 - 2**31


def arithmetic_reference(op, a, b=0):
    if op in ("DIV", "MOD"):
        if b == 0:
            raise ValueError("division by zero")
        quotient = abs(a) // abs(b)
        if (a < 0) != (b < 0):
            quotient = -quotient
        if op == "MOD":
            return a - quotient * b
        if quotient > 2147483647:
            raise ValueError("division overflow")
        return quotient
    if op in ("SHL", "SHR", "SAR"):
        if not 0 <= b <= 31:
            raise ValueError("shift count must be in 0..31")
        return i32(a << b) if op == "SHL" else (a & 0xffffffff) >> b if op == "SHR" else a >> b
    return {"AND": lambda: i32(a & b), "OR": lambda: i32(a | b), "XOR": lambda: i32(a ^ b),
            "NOT": lambda: i32(~a), "LAND": lambda: int(bool(a) and bool(b)),
            "LOR": lambda: int(bool(a) or bool(b)), "LXOR": lambda: int(bool(a) != bool(b)),
            "LNOT": lambda: int(not a)}[op]()


class CliTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="cli-", dir=WORK)
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def source(self, text, name="test.dna"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def call(self, *args, code=0, input=None, env=None):
        proc = subprocess.run([str(EXE), *map(str, args)], cwd=self.root, input=input,
                              capture_output=True, encoding="utf-8", timeout=10, env=env)
        self.assertEqual(proc.returncode, code, proc.stdout + proc.stderr)
        return proc

    def test_array_parameter_validation(self):
        def program(body, helper="PARAM p r0\nRET r0 _\n", arity=1):
            return ("GENE main i32\n" + body + "RET r0 _\nEND _ _\n"
                    + f"GENE f i32/{arity}\n" + helper + "END _ _\n")
        cases = [
            (program("ARRAY a 1\nLOAD r0 1\nCALL f r0\n"), "requires an array argument"),
            (program("ARRAY a 1\nLOAD r0 1\nCALL f a,r0\n", "RET r0 _\n"), "does not accept an array"),
            (program("VAR a 1\nLOAD r0 1\nCALL f a,r0\n"), "requires an array"),
            (program("LOAD r0 1\nCALL f missing,r0\n"), "undefined local missing"),
            (program("ARRAY a 1\nCALL f a,r7\n"), "uninitialized register r7"),
            (program("ARRAY a 1\nLOAD r0 1\nCALL f a,r0,r7\n", arity=2), "uninitialized register r7"),
            (program("ARRAY a 1\nLOAD r0 1\nCALL f a,r0\n", arity=2), "expects 2 arguments"),
            (program("PARAM a r0\nLOAD r0 0\n"), "main cannot declare PARAM"),
            (program("LOAD r0 0\n", arity=0), "PARAM requires i32/N with N in 1..4"),
            (program("LOAD r0 0\n", "PARAM p r1\nRET r0 _\n", 2), "PARAM requires '<name> r0'"),
            (program("LOAD r0 0\n", "PARAM r0 r0\nRET r0 _\n"), "invalid local name"),
            (program("LOAD r0 0\n", "PARAM _ r0\nRET r0 _\n"), "invalid local name"),
            (program("LOAD r0 0\n", "PARAM p r0\nPARAM q r0\nRET r0 _\n"), "only one PARAM"),
            (program("LOAD r0 0\n", "PARAM p r0\nVAR p 0\nRET r0 _\n"), "duplicate local"),
            (program("LOAD r0 0\n", "ARRAY p 1\nPARAM p r0\nRET r0 _\n"), "duplicate local"),
            (program("LOAD r0 0\n", "LOAD r0 0\nPARAM p r0\nRET r0 _\n"), "must precede"),
            (program("LOAD r0 0\n", "LABEL here _\nPARAM p r0\nRET r0 _\n"), "must precede"),
            (program("LOAD r0 0\n", "PARAM p r0\nGET r0 p\nRET r0 _\n"), "requires a variable"),
            (program("LOAD r0 0\n", "PARAM p r0\nSET p r0\nRET r0 _\n"), "requires a variable"),
            (program("LOAD r0 0\n", "PARAM p\nRET r0 _\n"), "exactly 3 fields"),
            # 0.14: recursion is legal; a dead self-caller compiles and never runs.
            (program("LOAD r0 0\n", "PARAM p r0\n" + "VAR v 0\n"), "must end with RET"),
        ]
        for operands in ("a", "a,", "a,_", "a,r0,", "a,b,r0", "_,r0"):
            cases.append((program(f"ARRAY a 1\nLOAD r0 1\nCALL f {operands}\n"), "CALL operands"))
        cases.append((program("ARRAY a 1\nLOAD r0 1\nCALL f a, r0\n"), "exactly 3 fields"))
        cases.append((program("ARRAY a 1\nLOAD r0 1\nCALL f r0,r1,r2,r3,r4\n"), "at most four register arguments"))
        cases.append((program("ARRAY a 1\nLOAD r0 1\nCALL f a,r0,r1,r2,r3,r4\n"), "at most four register arguments"))
        cases.append((program("ARRAY a 1\nLOAD r0 1\nCALL f a,r0,r1,r2\n"), "expects 1 arguments, got 3"))
        for source, message in cases:
            with self.subTest(message=message, source=source):
                data = json.loads(self.call("check", self.source(source), "--json", code=1).stdout)
                self.assertEqual(data["error"]["stage"], "compile")
                self.assertIn(message, data["error"]["message"])

    def test_array_parameter_dump_and_csv(self):
        source = self.source("GENE main i32/1\nARRAY values 8\nCALL size values,r0\nRET r0 _\nEND _ _\n"
                             "GENE size i32/1\nPARAM borrowed r0\nLEN r0 borrowed\nRET r0 _\nEND _ _\n")
        dumped = self.call("dump", source).stdout
        self.assertIn("PARAM borrowed r0", dumped)
        self.assertIn("CALL size values,r0", dumped)
        roundtrip = self.source(dumped, "dump.dna")
        for command in ("run", "interpret"):
            self.assertEqual(self.call(command, roundtrip, 3).stdout.strip(), "3")
        cases = self.source("arg1,expected\n0,0\n8,8\n9,0\n-1,0\n3,3\n", "arrays.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["summary"]["passed"], 3)
        self.assertEqual(data["summary"]["errors"], 2)
        self.assertIn("CALL array length 9", data["cases"][2]["error"]["message"])

    def test_array_pipeline_matches_reference(self):
        rng = random.Random(808)
        datasets = [[], [0], [7] * 24, [-2147483648, 2147483647, 0, -1, 1], list(range(255, -1, -1))]
        datasets += [[rng.randint(-10000, 10000) for _ in range(rng.randrange(129))] for _ in range(40)]
        for number, values in enumerate(datasets):
            threshold = [-2147483648, 2147483647, 0, -1, 5][number % 5]
            expected = sorted(v for v in values if v >= threshold)
            input_path = self.source("".join(f"{v}\n" for v in values), "исходные числа.txt")
            for command in ("run", "interpret"):
                with self.subTest(number=number, command=command):
                    output = self.root / f"{command}-{number}.txt"
                    data = json.loads(self.call(command, EXAMPLES / "list_pipeline.dna", threshold,
                                                "--input", input_path, "--output", output, "--json").stdout)
                    self.assertEqual(data["result"], len(expected))
                    self.assertEqual(data["output_count"], len(expected))
                    self.assertEqual(output.read_bytes(), "".join(f"{v}\n" for v in expected).encode())

    def test_array_library_prefixes_and_repeated_calls(self):
        # Import via a local Unicode path and verify untouched tails against Python.
        self.source((EXAMPLES / "lists.dna").read_text(encoding="utf-8"), "модуль.dna")
        source = self.source("IMPORT модуль.dna _\nGENE main i32/2\nARRAY a 64\n"
                             "READ a r7\nCALL filter a,r0,r1\n"
                             "CALL sort a,r0\nCALL sum a,r0\nWRITE a r7\nRET r0 _\nEND _ _\n")
        rng = random.Random(809)
        for case in range(40):
            values = [rng.randint(-2147483648, 2147483647) for _ in range(rng.randrange(65))]
            length = rng.randrange(len(values) + 1)
            threshold = rng.randint(-2147483648, 2147483647)
            kept = [v for v in values[:length] if v >= threshold]
            expected = sorted(kept) + values[len(kept):]
            input_path = self.source("".join(f"{v}\n" for v in values), "numbers.txt")
            for command in ("run", "interpret"):
                output = self.root / f"prefix-{case}-{command}.txt"
                data = json.loads(self.call(command, source, length, threshold, "--input", input_path,
                                            "--output", output, "--json").stdout)
                self.assertEqual(data["result"], i32(sum(kept)))
                self.assertEqual([int(v) for v in output.read_text().splitlines()], expected)

    def test_array_parameter_failure_preserves_output(self):
        source = self.source("GENE main i32/2\nARRAY a 8\nREAD a r2\nWRITE a r2\n"
                             "CALL modify a,r0,r1\nRET r0 _\nEND _ _\n"
                             "GENE modify i32/2\nPARAM p r0\nLOAD r2 123\nLOAD r3 0\n"
                             "ASET p r3,r2\nWRITE p r0\nASET p r1,r2\nRET r0 _\nEND _ _\n")
        input_path = self.source("7\n8\n9\n", "input.txt")
        for command in ("run", "interpret"):
            for length, index, error in [(2, 2, "array index 2"), (0, 0, "array is empty"),
                                          (2, -1, "array index -1"), (-1, 0, "CALL array length -1"),
                                          (9, 0, "CALL array length 9")]:
                output = self.root / f"fail-{command}-{length}-{index}.txt"
                data = json.loads(self.call(command, source, length, index, "--input", input_path,
                                            "--output", output, "--json", code=1).stdout)
                self.assertIn(error, data["error"]["message"])
                self.assertEqual(data["error"]["stage"], "run")
                self.assertFalse(output.exists())
        self.assertEqual(input_path.read_text(), "7\n8\n9\n")
        self.assertFalse(list(self.root.glob("*.tmp")))

    def test_help_and_version(self):
        self.assertIn("Codons 0.16.0", self.call("--version").stdout)
        self.assertIn("codons new", self.call().stdout)
        self.assertIn("--json", self.call("--help").stdout)

    def test_create_run_unicode_and_spaces(self):
        folder = self.root / "Мои программы"
        folder.mkdir()
        source = folder / "сложение.dna"
        data = json.loads(self.call("new", source, "--json").stdout)
        self.assertEqual(data, {"ok": True, "file": str(source)})
        self.assertEqual(self.call("run", source, 10, 20).stdout.strip(), "30")
        self.assertEqual(self.call("interpret", source, -7, 4).stdout.strip(), "-3")

    def test_new_preserves_existing_file(self):
        source = self.source("existing content")
        data = json.loads(self.call("new", source, "--json", code=1).stdout)
        self.assertIn("already exists", data["error"]["message"])
        self.assertEqual(source.read_text(), "existing content")

    def test_input_arity_and_literals(self):
        source = self.source("GENE main i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n")
        for args in [[], [1], [1, 2, 3], ["2147483648", 0], ["-2147483649", 0],
                     ["1.2", 0], ["0x10", 0], [" 1", 0], ["--unknown"], ["+", 0]]:
            with self.subTest(args=args):
                data = json.loads(self.call("run", source, *args, "--json", code=2).stdout)
                self.assertFalse(data["ok"])
                self.assertEqual(data["error"]["stage"], "arguments")
        self.assertEqual(self.call("run", source, "2147483647", 1).stdout.strip(), "-2147483648")

    def test_comparisons_all_boundaries(self):
        values = [-2147483648, -10, -1, 0, 1, 10, 2147483647]
        operations = {"EQ": lambda a, b: int(a == b), "LT": lambda a, b: int(a < b), "MIN": min, "MAX": max}
        for op, expected in operations.items():
            source = self.source(f"GENE main i32/2\n{op} r0 r1\nRET r0 _\nEND _ _\n")
            for a in values:
                for b in values:
                    with self.subTest(op=op, a=a, b=b):
                        self.assertEqual(int(self.call("run", source, a, b).stdout), expected(a, b))
                        self.assertEqual(int(self.call("interpret", source, a, b).stdout), expected(a, b))

    def test_check_and_interpret_have_no_jit(self):
        source = self.source("GENE main i32/1\nLOAD r1 -3\nMUL r0 r1\nRET r0 _\nEND _ _\n")
        checked = json.loads(self.call("check", source, "--json").stdout)
        self.assertEqual(checked["stats"]["arity"], 1)
        self.assertEqual(checked["stats"]["execution_steps"], 3)
        self.assertEqual(checked["stats"]["code_bytes"], 0)
        for command in ("run", "interpret"):
            data = json.loads(self.call(command, source, 7, "--json").stdout)
            self.assertEqual(data["result"], -21)
            self.assertEqual(data["stats"]["code_bytes"] == 0, command == "interpret")

    def test_compile_error_is_json(self):
        source = self.source("GENE main i32\nRET r0 _\nEND _ _\n", 'ошибка.dna')
        proc = self.call("run", source, "--json", code=1)
        data = json.loads(proc.stdout)
        self.assertEqual(proc.stderr, "")
        self.assertEqual(data["error"]["stage"], "compile")
        self.assertIn("ошибка.dna:2:", data["error"]["message"])

    def test_json_missing_file_and_unknown_command(self):
        data = json.loads(self.call("check", "missing.dna", "--json", code=1).stdout)
        self.assertIn("cannot open", data["error"]["message"])
        data = json.loads(self.call("unknown", "x", "--json", code=2).stdout)
        self.assertEqual(data["error"]["stage"], "arguments")

    def test_json_stays_valid_for_damaged_source_and_long_arguments(self):
        source = self.root / "invalid.dna"
        source.write_bytes(b"GENE main i32\n\xff r0 r0\n")
        data = json.loads(self.call("check", source, "--json", code=1).stdout)
        self.assertIn("valid UTF-8", data["error"]["message"])
        for suffix in ("", "a", "ab", "abc"):
            command = suffix + "\U0001f9ec" * 500
            data = json.loads(self.call(command, "x", "--json", code=2).stdout)
            self.assertFalse(data["ok"])

    def test_unicode_imports_deduplicate_case(self):
        self.source("GENE sum i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n", "Каталог/модуль.dna")
        source = self.source("IMPORT модуль.dna _\nIMPORT МОДУЛЬ.dna _\nGENE main i32/2\nCALL sum r0,r1\nRET r0 _\nEND _ _\n", "Каталог/main.dna")
        data = json.loads(self.call("run", source, 11, 31, "--json").stdout)
        self.assertEqual(data["result"], 42)
        self.assertEqual(data["stats"]["files"], 2)

    def test_dump_round_trip(self):
        source = self.source("GENE main i32\nCALL helper _\nRET r0 _\nEND _ _\nGENE helper i32\nLOAD r7 -2147483648\nRET r7 _\nEND _ _\n")
        normalized = self.source(self.call("dump", source).stdout, "normalized.dna")
        self.assertEqual(self.call("run", normalized).stdout.strip(), "-2147483648")

    def test_strings_operations_agree_across_engines(self):
        source = self.source("GENE main i32\nSLOAD r1 \"hello\"\nSLOAD r2 \" world\"\nSCAT r3 r1,r2\n"
                             "SLOAD r4 \"hello world\"\nSEQ r5 r3,r4\nSLEN r6 r3\nADD r6 r5\n"
                             "LOAD r7 10\nSGET r0 r3,r7\nADD r6 r0\nRET r6 _\nEND _ _\n")
        # 11 (length) + 1 (equal) + 100 ('d') = 112
        for command in ("run", "interpret", "verify"):
            with self.subTest(command=command):
                data = json.loads(self.call(command, source, "--json").stdout)
                self.assertTrue(data["ok"])
                self.assertEqual(data["result"], 112)

    def test_strings_escapes_and_dump_round_trip(self):
        text = ("GENE main i32\nSLOAD r1 \"a\\n\\t\\\\\\\"\\x01# b\"\nSLOAD r2 \"plain\"\n"
                "SCAT r3 r1,r2\nSLEN r4 r3\nLOAD r5 13\nSGET r6 r3,r5\nADD r4 r6\nRET r4 _\nEND _ _\n")
        source = self.source(text)
        for command in ("run", "interpret"):
            self.assertEqual(int(self.call(command, source).stdout), 14 + ord("n"))
        dumped = self.call("dump", source).stdout
        self.assertIn('SLOAD r1 "a\\n\\t\\\\\\"\\x01# b"', dumped)
        normalized = self.source(dumped, "normalized.dna")
        self.assertEqual(self.call("dump", normalized).stdout, dumped)
        for command in ("run", "interpret", "verify"):
            self.assertEqual(int(self.call(command, normalized).stdout), 14 + ord("n"))

    def test_strings_faults_match_across_engines(self):
        cases = [
            ("GENE main i32\nLOAD r1 99\nSLEN r2 r1\nRET r2 _\nEND _ _\n", 3,
             "invalid string handle 99 (0 handles exist in this invocation)"),
            ("GENE main i32\nSLOAD r1 \"ab\"\nLOAD r2 -1\nSLEN r3 r2\nRET r3 _\nEND _ _\n", 4,
             "invalid string handle -1 (1 handles exist in this invocation)"),
            ("GENE main i32\nSLOAD r1 \"ab\"\nLOAD r2 2\nSGET r3 r1,r2\nRET r3 _\nEND _ _\n", 4,
             "string index 2 out of bounds: valid indices are 0..1 (length 2)"),
            ("GENE main i32\nSLOAD r1 \"\"\nLOAD r2 0\nSGET r3 r1,r2\nRET r3 _\nEND _ _\n", 4,
             "string index 0 out of bounds: the string is empty (length 0)"),
            ("GENE main i32\nSLOAD r1 \"\"\nLOAD r3 1\nLOAD r4 5000\nLABEL loop _\n"
             "SCAT r1 r1,r1\nSUB r4 r3\nJNZ r4 loop\nRET r1 _\nEND _ _\n", 6,
             "string handle table full (4096 entries per invocation)"),
            ("GENE main i32\nSLOAD r1 \"a\"\nLOAD r3 1\nLOAD r4 17\nLABEL grow _\n"
             "SCAT r1 r1,r1\nSUB r4 r3\nJNZ r4 grow\nMOV r2 r1\nLOAD r4 8\nLABEL fill _\n"
             "SCAT r2 r2,r1\nSUB r4 r3\nJNZ r4 fill\nSLEN r5 r2\nRET r5 _\nEND _ _\n", 12,
             "string memory exhausted"),
        ]
        for text, line, message in cases:
            with self.subTest(message=message):
                source = self.source(text)
                messages = []
                for command in ("run", "interpret"):
                    data = json.loads(self.call(command, source, "--json", code=1).stdout)
                    self.assertEqual(data["error"]["stage"], "run")
                    self.assertTrue(data["error"]["message"].startswith(f"{source}:{line}: "),
                                    data["error"]["message"])
                    self.assertIn(message, data["error"]["message"])
                    messages.append(data["error"]["message"])
                self.assertEqual(*messages)
                verify = self.call("verify", source, code=1)
                self.assertIn("verified: JIT and interpreter agree on the failure", verify.stderr)
                self.assertIn(message, verify.stderr)

    def test_print_streams_capture_and_verify(self):
        source = self.source("GENE main i32\nSLOAD r1 \"hi\"\nPRINT r1 _\nSLOAD r2 \" there\\n\"\nPRINT r2 _\n"
                             "SLEN r3 r1\nRET r3 _\nEND _ _\n")
        for command in ("run", "interpret"):
            proc = self.call(command, source)
            self.assertEqual(proc.stdout.replace("\r\n", "\n"), "hi there\n2\n")
        for command in ("run", "interpret", "verify"):
            data = json.loads(self.call(command, source, "--json").stdout)
            self.assertTrue(data["ok"])
            self.assertEqual(data["printed"], "hi there\n")
            self.assertEqual(data["result"], 2)
        # verify prints the agreed bytes once in text mode.
        proc = self.call("verify", source)
        self.assertEqual(proc.stdout.replace("\r\n", "\n"), "hi there\n2\n")
        # No PRINT, no output: the JSON field is present and empty.
        plain = self.source("GENE main i32\nLOAD r0 1\nRET r0 _\nEND _ _\n", "plain.dna")
        data = json.loads(self.call("run", plain, "--json").stdout)
        self.assertEqual(data["printed"], "")

    def test_recursion_runtime_limits(self):
        fact = self.source(
            "GENE fact i32/1\nLOAD r1 1\nJZ r0 base\nMOV r2 r0\nSUB r0 r1\nCALL fact r0\n"
            "MUL r2 r0\nMOV r0 r2\nRET r0 _\nLABEL base _\nLOAD r0 1\nRET r0 _\nEND _ _\n"
            "GENE main i32/1\nCALL fact r0\nRET r0 _\nEND _ _\n")
        for command in ("run", "interpret"):
            self.assertEqual(self.call(command, fact, "5").stdout.strip(), "120")
        self.assertEqual(self.call("verify", fact, "5").stdout.strip(), "120")
        data = json.loads(self.call("check", fact, "--json").stdout)
        self.assertTrue(data["stats"]["dynamic_steps"])
        # Dead code may recurse: the cycle in pruned gene f never executes.
        dead = self.source("GENE main i32\nLOAD r0 0\nRET r0 _\nEND _ _\n"
                           "GENE f i32/1\nPARAM p r0\nCALL f p,r0\nRET r0 _\nEND _ _\n", "dead.dna")
        self.assertEqual(self.call("run", dead).stdout.strip(), "0")
        # Unbounded recursion dies identically in both engines, with file and line.
        bomb = self.source("GENE main i32\nCALL main _\nRET r0 _\nEND _ _\n", "bomb.dna")
        messages = []
        for command in ("run", "interpret"):
            data = json.loads(self.call(command, bomb, "--json", code=1).stdout)
            self.assertIn("call depth limit reached (256 active genes)", data["error"]["message"])
            self.assertIn(f"{bomb}:2:", data["error"]["message"])
            messages.append(data["error"]["message"])
        self.assertEqual(*messages)
        # Large per-frame arrays exhaust the 1 MiB frame pool before the depth limit.
        frames = self.source("GENE rec i32/1\nARRAY buf 4096\nLOAD r1 1\nSUB r0 r1\nCALL rec r0\nRET r0 _\nEND _ _\n"
                             "GENE main i32/1\nCALL rec r0\nRET r0 _\nEND _ _\n", "frames.dna")
        for command in ("run", "interpret"):
            data = json.loads(self.call(command, frames, "1000000", "--json", code=1).stdout)
            self.assertIn("call frame memory exhausted", data["error"]["message"])
            self.assertIn(f"{frames}:5:", data["error"]["message"])

    def test_wide_registers(self):
        # 2^62 + 2^62 wraps to INT64_MIN; the flag reaches i32 via WEQ.
        wrap = self.source("GENE main i32\nWLOAD w0 4611686018427387904\nWADD w0 w0\n"
                           "WLOAD w1 -9223372036854775808\nWEQ r1 w0,w1\nRET r1 _\nEND _ _\n",
                           "wide.dna")
        for command in ("run", "interpret"):
            self.assertEqual(self.call(command, wrap).stdout.strip(), "1")
        self.assertEqual(self.call("verify", wrap).stdout.strip(), "1")
        # Wide faults carry file and line and match between the engines.
        for body, needle in (
            ("WLOAD w0 1\nWLOAD w1 0\nWDIV w0 w1", "division by zero"),
            ("WLOAD w0 -9223372036854775808\nWLOAD w1 -1\nWDIV w0 w1",
             "division overflow: -9223372036854775808 / -1 does not fit i64"),
            ("WLOAD w0 1\nWLOAD w1 64\nWSHL w0 w1", "wide shift count must be in 0..63"),
            ("WLOAD w0 1\nWLOAD w0 1099511627776\nNARROW r1 w0",
             "wide value does not fit in i32 (-2147483648..2147483647)"),
        ):
            path = self.source(f"GENE main i32\n{body}\nLOAD r0 0\nRET r0 _\nEND _ _\n", "wfault.dna")
            messages = []
            for command in ("run", "interpret"):
                data = json.loads(self.call(command, path, "--json", code=1).stdout)
                self.assertIn(needle, data["error"]["message"])
                self.assertIn(f"{path}:4:", data["error"]["message"])
                messages.append(data["error"]["message"])
            self.assertEqual(*messages)
        # Compile-time checks.
        uninit = self.source("GENE main i32\nWADD w0 w1\nLOAD r0 0\nRET r0 _\nEND _ _\n", "wuninit.dna")
        self.assertIn("read of uninitialized wide register w0",
                      self.call("check", uninit, code=1).stderr)
        # Dump round trip keeps the extreme literals exact: INT64_MIN + INT64_MAX = -1.
        edge = self.source("GENE main i32\nWLOAD w0 -9223372036854775808\n"
                           "WLOAD w1 9223372036854775807\nWADD w0 w1\nNARROW r0 w0\nRET r0 _\nEND _ _\n",
                           "wedge.dna")
        dumped = self.root / "wedge-dump.dna"
        dumped.write_text(self.call("dump", edge).stdout.replace("\r\n", "\n"), encoding="utf-8")
        self.assertEqual(self.call("verify", str(dumped)).stdout.strip(), "-1")
        # WNOT with a dropped '_' is repairable.
        broken = self.source("GENE main i32\nWLOAD w0 0\nWNOT w0\nNARROW r0 w0\nRET r0 _\nEND _ _\n",
                             "wbroken.dna")
        data = json.loads(self.call("run", broken, "--repair", "--json").stdout)
        self.assertTrue(data["ok"])
        self.assertEqual(data["result"], -1)
        self.assertEqual(data["repair_log"][0]["kind"], "fill")

    def test_registers_beyond_r7(self):
        # The register file reaches r255 and travels through calls and recursion.
        high = self.source("GENE main i32\nLOAD r15 40\nLOAD r200 2\nADD r15 r200\n"
                           "RET r15 _\nEND _ _\n", "high.dna")
        for command in ("run", "interpret"):
            self.assertEqual(self.call(command, high).stdout.strip(), "42")
        self.assertEqual(self.call("verify", high).stdout.strip(), "42")
        call = self.source("GENE main i32\nLOAD r12 30\nLOAD r13 12\nCALL add2 r12,r13\n"
                           "RET r0 _\nEND _ _\nGENE add2 i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n",
                           "rcall.dna")
        for command in ("run", "interpret"):
            self.assertEqual(self.call(command, call).stdout.strip(), "42")
        # Multi-digit registers survive the dump round trip.
        dumped = self.root / "rcall-dump.dna"
        dumped.write_text(self.call("dump", call).stdout.replace("\r\n", "\n"), encoding="utf-8")
        self.assertEqual(self.call("verify", str(dumped)).stdout.strip(), "42")
        # Out of range and uninitialized registers fail at compile time.
        over = self.source("GENE main i32\nLOAD r256 1\nRET r256 _\nEND _ _\n", "over.dna")
        self.assertIn("first operand must be a register r0..r255",
                      self.call("check", over, code=1).stderr)
        uninit = self.source("GENE main i32\nRET r9 _\nEND _ _\n", "runinit.dna")
        self.assertIn("read of uninitialized register r9",
                      self.call("check", uninit, code=1).stderr)

    def test_struct_sugar(self):
        # STRUCT expands to one-cell locals with dotted names.
        point = self.source("GENE main i32\nSTRUCT p x,y\nLOAD r1 40\nLOAD r2 2\nSET p.x r1\n"
                            "SET p.y r2\nGET r3 p.x\nGET r4 p.y\nADD r3 r4\nRET r3 _\nEND _ _\n",
                            "point.dna")
        for command in ("run", "interpret"):
            self.assertEqual(self.call(command, point).stdout.strip(), "42")
        self.assertEqual(self.call("verify", point).stdout.strip(), "42")
        # The dump normalizes STRUCT into VAR lines that reparse unchanged.
        dumped = self.root / "point-dump.dna"
        text = self.call("dump", point).stdout.replace("\r\n", "\n")
        dumped.write_text(text, encoding="utf-8")
        self.assertIn("VAR p.x 0", text)
        self.assertIn("VAR p.y 0", text)
        self.assertEqual(self.call("verify", str(dumped)).stdout.strip(), "42")
        # The bare struct name is not a local; fields are unique.
        bare = self.source("GENE main i32\nSTRUCT p x\nGET r0 p\nRET r0 _\nEND _ _\n", "bare.dna")
        self.assertIn("undefined local p in gene main", self.call("check", bare, code=1).stderr)
        dup = self.source("GENE main i32\nSTRUCT p x,x\nRET r0 _\nEND _ _\n", "dup.dna")
        self.assertIn("duplicate local declaration", self.call("check", dup, code=1).stderr)
        late = self.source("GENE main i32\nLOAD r0 1\nSTRUCT p x\nRET r0 _\nEND _ _\n", "late.dna")
        self.assertIn("must precede all instructions", self.call("check", late, code=1).stderr)

    def test_literal_operands(self):
        # A literal second operand works for every binary i32 op (0.17).
        src = self.source("GENE main i32/1\nADD r0 10\nSHL r0 2\nMIN r0 100\nMOD r0 7\nRET r0 _\nEND _ _\n",
                          "lit.dna")
        self.assertEqual(self.call("run", src, "5").stdout.strip(), "4")  # ((5+10)<<2 min 100) % 7
        data = json.loads(self.call("verify", src, "5", "--json").stdout)
        self.assertTrue(data["verified"])
        self.assertEqual(data["result"], 4)
        # Literal faults are compile errors and carry the hint plus line_text.
        bad = self.source("GENE main i32\nLOAD r0 1\nDIV r0 0\nRET r0 _\nEND _ _\n", "lit-bad.dna")
        data = json.loads(self.call("check", bad, "--json", code=1).stdout)
        self.assertIn("division by literal zero", data["error"]["message"])
        self.assertEqual(data["error"]["line_text"], "DIV r0 0")
        # The dump prints the literal form and it reparses.
        text = self.call("dump", src).stdout
        self.assertIn("ADD r0 10", text)

    def test_error_hints_and_line_text(self):
        # JSON errors carry the offending source line, so an agent sees what it wrote.
        src = self.source("GENE main i32\nLOAD r0 1\nADD r0 r1\nRET r0 _\nEND _ _\n", "hint.dna")
        data = json.loads(self.call("check", src, "--json", code=1).stdout)
        self.assertIn("read of uninitialized register r1", data["error"]["message"])
        self.assertIn("LOAD r1 0", data["error"]["message"])
        self.assertEqual(data["error"]["line_text"], "ADD r0 r1")
        # The comma-space slip (CALL add r1, r2) gets an actionable hint.
        comma = self.source("GENE add i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n"
                            "GENE main i32\nLOAD r1 1\nLOAD r2 2\nCALL add r1, r2\nRET r0 _\nEND _ _\n",
                            "comma.dna")
        data = json.loads(self.call("check", comma, "--json", code=1).stdout)
        self.assertIn("never put a space after a comma", data["error"]["message"])
        self.assertEqual(data["error"]["line_text"], "CALL add r1, r2")
        # A comma inside a field points at the same rule.
        inner = self.source("GENE main i32\nLOAD r0 1\nLOAD r1 2\nADD r0, r1\nRET r0 _\nEND _ _\n",
                            "inner.dna")
        self.assertIn("no comma here", self.call("check", inner, code=1).stderr)
        # Undefined names explain how to declare them.
        undef = self.source("GENE main i32\nLOAD r0 1\nCALL oops r0\nRET r0 _\nEND _ _\n", "undef.dna")
        self.assertIn("GENE oops", self.call("check", undef, code=1).stderr)
        # line_text stays absent (and JSON valid) for a damaged source.
        damaged = self.root / "damaged.dna"
        damaged.write_bytes(b"GENE main i32\n\xff\xfe\n")
        data = json.loads(self.call("check", damaged, "--json", code=1).stdout)
        self.assertNotIn("line_text", data["error"])

    def test_print_faults_and_repair(self):
        source = self.source("GENE main i32\nLOAD r1 7\nPRINT r1 _\nRET r1 _\nEND _ _\n")
        messages = []
        for command in ("run", "interpret"):
            data = json.loads(self.call(command, source, "--json", code=1).stdout)
            self.assertEqual(data["error"]["stage"], "run")
            self.assertIn("invalid string handle 7 (0 handles exist in this invocation)",
                          data["error"]["message"])
            self.assertIn(f"{source}:3:", data["error"]["message"])
            messages.append(data["error"]["message"])
        self.assertEqual(*messages)
        # The third field is unambiguous: --repair restores it.
        broken = self.source("GENE main i32\nSLOAD r1 \"ok\"\nPRINT r1\nLOAD r2 3\nRET r2 _\nEND _ _\n", "broken.dna")
        data = json.loads(self.call("run", broken, "--repair", "--json").stdout)
        self.assertTrue(data["ok"])
        self.assertEqual(data["printed"], "ok")
        self.assertEqual(data["result"], 3)
        self.assertEqual(data["repair_log"][0]["kind"], "fill")

    def test_strings_rejections(self):
        cases = [
            ("GENE main i32\nSLOAD r1 hello\nRET r1 _\nEND _ _\n", "requires a quoted literal", 2),
            ("GENE main i32\nSLOAD r1 \"a\\qb\"\nRET r1 _\nEND _ _\n", "unknown escape", 2),
            ("GENE main i32\nSLOAD r1 \"unterminated\nRET r1 _\nEND _ _\n", "requires a quoted literal", 2),
            ("GENE main i32\nSLOAD r1 \"a\" extra\nRET r1 _\nEND _ _\n", "exactly 3 fields", 2),
            ("GENE main i32\nSLEN r1 r2,r3\nRET r1 _\nEND _ _\n", "SLEN requires 'rN rM'", 2),
            ("GENE main i32\nSCAT r1 r2\nRET r1 _\nEND _ _\n", "requires 'rN rM,rK'", 2),
            ("GENE main i32\nSLOAD r1 \"x\"\nSLEN r2 r7\nRET r2 _\nEND _ _\n", "uninitialized register r7", 3),
        ]
        for text, message, line in cases:
            with self.subTest(message=message):
                source = self.source(text)
                data = json.loads(self.call("check", source, "--json", code=1).stdout)
                self.assertEqual(data["error"]["stage"], "compile")
                self.assertIn(message, data["error"]["message"])
                self.assertIn(f"{source}:{line}:", data["error"]["message"])

    def test_repair_is_explicit(self):
        source = self.source("GENE main i32\nLOAD r0 42\nRET r0\nEND _\n")
        self.call("run", source, code=1)
        data = json.loads(self.call("run", source, "--repair", "--json").stdout)
        self.assertEqual(data["result"], 42)
        self.assertEqual(data["stats"]["repairs"], 2)
        self.assertIn("RET r0\n", source.read_text())

        broken = self.source("GENE main i32\nCALL absent _\nRET r0 _\nEND _ _\n", "repair-link.dna")
        self.assertIn("undefined gene", self.call("check", broken, code=1).stderr)
        report = json.loads(self.call("check", broken, "--repair", "--json").stdout)
        self.assertEqual(report["stats"]["quarantines"], 1)
        self.assertEqual(report["repair_log"][0]["line"], 2)
        self.assertEqual(report["repair_log"][0]["kind"], "quarantine")
        self.assertIn("CALL target", report["repair_log"][0]["detail"])
        for command in ("run", "interpret"):
            failed = json.loads(self.call(command, broken, "--repair", "--json", code=1).stdout)
            self.assertEqual(failed["error"]["stage"], "run")
            self.assertIn("quarantined repaired codon", failed["error"]["message"])

    def test_live_inputs_and_failed_update(self):
        source = self.source("GENE main i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n", "old.dna")
        updated = self.source("GENE main i32/2\nMUL r0 r1\nRET r0 _\nEND _ _\n", "new.dna")
        invalid = self.source("GENE main i32/1\nRET r0 _\nEND _ _\n", "bad.dna")
        proc = self.call("live", source, input=f"run 10 20\nreload {updated}\nrun 10 20\nreload {invalid}\nrun 10 20\nrun\nquit\n")
        self.assertEqual([x for x in proc.stdout.splitlines() if x.startswith("result=")],
                         ["result=30", "result=200", "result=200"])
        self.assertIn("incompatible signature", proc.stderr)
        self.assertIn("expects 2 arguments", proc.stderr)

    def test_verify_agrees(self):
        source = self.source("GENE main i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n")
        proc = self.call("verify", source, 10, 20)
        self.assertEqual(proc.stdout.strip(), "30")
        self.assertIn("verified: JIT and interpreter agree", proc.stderr)

    def test_verify_identical_failure(self):
        source = self.source("GENE main i32\nARRAY a 2\nLOAD r0 5\nAGET r1 a,r0\nRET r1 _\nEND _ _\n")
        proc = self.call("verify", source, code=1)
        self.assertIn("verified: JIT and interpreter agree on the failure", proc.stderr)
        self.assertIn("array index 5 out of bounds", proc.stderr)
        self.assertNotIn("DIVERGENCE", proc.stderr)

    def test_verify_json(self):
        source = self.source("GENE main i32/1\nMUL r0 r0\nRET r0 _\nEND _ _\n")
        data = json.loads(self.call("verify", source, 7, "--json").stdout)
        self.assertTrue(data["ok"])
        self.assertTrue(data["verified"])
        self.assertEqual(data["result"], 49)

    def test_verify_lists(self):
        source = self.source("GENE main i32\nARRAY items 8\nREAD items r0\nWRITE items r0\nRET r0 _\nEND _ _\n")
        list_in = self.source("3\n-1\n4\n", "in.txt")
        out = self.root / "out.txt"
        proc = self.call("verify", source, "--input", list_in, "--output", out)
        self.assertEqual(proc.stdout.strip(), "3")
        self.assertEqual(out.read_text(encoding="utf-8").split(), ["3", "-1", "4"])
        self.assertIn("verified", proc.stderr)

    def test_four_arguments_and_inline_output_list(self):
        source = self.source("GENE main i32/4\nADD r0 r1\nADD r0 r2\nADD r0 r3\nRET r0 _\nEND _ _\n")
        for command in ("run", "interpret", "verify"):
            with self.subTest(command=command):
                self.assertEqual(self.call(command, source, 1, 2, 3, 4).stdout.strip(), "10")
        self.call("run", source, 1, 2, 3, 4, 5, code=2)  # five inputs are rejected
        csv = self.source("arg1,arg2,arg3,arg4,expected\n1,2,3,4,10\n-1,-2,-3,-4,-10\n0,0,0,0,0\n", "four.csv")
        proc = self.call("test", source, csv)
        self.assertIn("Tests: 3 total, 3 passed", proc.stdout)

    def test_json_output_list_without_file(self):
        source = self.source("GENE main i32/1\nARRAY out 3\nLOAD r1 0\nASET out r1,r0\nLOAD r1 1\nASET out r1,r0\n"
                             "LOAD r1 2\nASET out r1,r0\nLOAD r1 3\nWRITE out r1\nRET r1 _\nEND _ _\n")
        for command in ("run", "interpret", "verify"):
            with self.subTest(command=command):
                data = json.loads(self.call(command, source, 7, "--json").stdout)
                self.assertTrue(data["ok"])
                self.assertEqual(data["output_list"], [7, 7, 7])
        # Text mode has no list channel: WRITE still requires --output there.
        proc = self.call("run", source, 7, code=1)
        self.assertIn("WRITE requires an output list", proc.stderr)

    def test_live_gated_reload(self):
        old = self.source("GENE main i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n", "old.dna")
        new = self.source("GENE main i32/2\nMUL r0 r1\nRET r0 _\nEND _ _\n", "new.dna")
        bad = self.source("GENE main i32/2\nSUB r0 r1\nRET r0 _\nEND _ _\n", "bad.dna")
        gate = self.source("arg1,arg2,expected\n10,20,200\n", "gate.csv")
        proc = self.call("live", old, input=(
            f"run 10 20\nreload {new} --gate {gate}\nrun 10 20\n"
            f"reload {bad} --gate {gate}\nrun 10 20\nreload {new} --gate\nquit\n"))
        results = [x for x in proc.stdout.splitlines() if x.startswith("result=")]
        self.assertEqual(results, ["result=30", "result=200", "result=200"])
        self.assertIn("updated (gate passed)", proc.stdout)
        self.assertIn("PASS", proc.stdout)
        self.assertIn("FAIL", proc.stdout)
        self.assertIn("Gate rejected the update; previous version is still active.", proc.stderr)
        self.assertIn("expected: reload <file.dna> [--gate <cases.csv>]", proc.stderr)

    def test_emit_utf8_path(self):
        source = self.source("GENE main i32\nLOAD r0 5\nRET r0 _\nEND _ _\n")
        output = self.root / "машинный код.bin"
        data = json.loads(self.call("emit", source, output, "--json").stdout)
        self.assertEqual(output.stat().st_size, data["stats"]["code_bytes"])
        self.assertEqual(data["stats"]["entry_offset"], 0)

    def test_branch_and_loop_examples(self):
        cases = [("sum.dna", [10], 55), ("sum.dna", [0], 0), ("sum.dna", [-5], 0),
                 ("factorial.dna", [5], 120), ("factorial.dna", [0], 1),
                 ("factorial.dna", [-1], 0), ("factorial.dna", [12], 479001600),
                 ("branch.dna", [-2147483648, 2147483647], 2147483647),
                 ("branch.dna", [7, -4], 7), ("branch.dna", [-3, -3], -3)]
        for name, args, expected in cases:
            for command in ("run", "interpret"):
                with self.subTest(name=name, args=args, command=command):
                    data = json.loads(self.call(command, EXAMPLES / name, *args, "--json").stdout)
                    self.assertEqual(data["result"], expected)
                    self.assertIsNone(data["stats"]["execution_steps"])
                    self.assertTrue(data["stats"]["dynamic_steps"])
                    self.assertEqual(data["stats"]["execution_limit"], 1000000)

    def test_infinite_loop_is_a_run_error(self):
        source = EXAMPLES / "infinite.dna"
        data = json.loads(self.call("check", source, "--json").stdout)
        self.assertTrue(data["ok"])
        self.assertEqual(data["stats"]["code_bytes"], 0)
        messages = []
        for command in ("run", "interpret"):
            data = json.loads(self.call(command, source, "--json", code=1).stdout)
            self.assertFalse(data["ok"])
            self.assertNotIn("result", data)
            self.assertEqual(data["error"]["stage"], "run")
            self.assertIn("execution budget exhausted (limit: 1000000 steps)", data["error"]["message"])
            messages.append(data["error"]["message"])
        self.assertEqual(messages[0], messages[1])

    def test_loop_dump_round_trip(self):
        source = EXAMPLES / "factorial.dna"
        normalized = self.source(self.call("dump", source).stdout, "normalized.dna")
        for command in ("run", "interpret"):
            self.assertEqual(self.call(command, normalized, 6).stdout.strip(), "720")
        def codons(text):
            return [line.split("#", 1)[0].strip() for line in text.splitlines() if line.strip()]
        self.assertEqual(codons(self.call("dump", normalized).stdout), codons(self.call("dump", source).stdout))

    def test_live_recovers_from_loop_limit(self):
        source = self.source("GENE main i32/1\nJZ r0 done\nLABEL spin _\nJMP spin _\n"
                             "LABEL done _\nLOAD r0 7\nRET r0 _\nEND _ _\n", "loop.dna")
        updated = self.source("GENE main i32/1\nLOAD r0 42\nRET r0 _\nEND _ _\n", "updated.dna")
        proc = self.call("live", source, input=f"run 1\nrun 0\nreload {updated}\nrun 1\nquit\n")
        self.assertIn("execution budget exhausted", proc.stderr)
        self.assertEqual([x for x in proc.stdout.splitlines() if x.startswith("result=")],
                         ["result=7", "result=42"])

    def test_nested_loops_and_long_jumps(self):
        padding = "LOAD r7 123\n" * 100
        source = self.source("GENE main i32/2\nLOAD r2 0\nLOAD r3 1\nLABEL outer _\n"
                             "JZ r0 done\nMOV r4 r1\nLABEL inner _\nJZ r4 next\n" + padding +
                             "ADD r2 r4\nSUB r4 r3\nJMP inner _\nLABEL next _\n"
                             "SUB r0 r3\nJMP outer _\nLABEL done _\nRET r2 _\nEND _ _\n")
        for a, b in [(7, 5), (0, 10), (10, 0), (1, 1)]:
            for command in ("run", "interpret"):
                self.assertEqual(int(self.call(command, source, a, b).stdout), a * b * (b + 1) // 2)

    def test_generated_loops_with_calls_and_branches(self):
        rng = random.Random(303)

        def i32(value):
            return (value + 2**31) % 2**32 - 2**31

        for case in range(100):
            count = rng.randrange(21)
            seed = rng.choice([-2147483648, 2147483647, rng.randrange(-1000, 1001)])
            threshold, factor, bias = rng.randrange(1, 21), rng.randrange(-3, 4), rng.randrange(-20, 21)
            expected = seed
            for counter in range(count, 0, -1):
                expected = i32(expected + counter) if counter < threshold else i32(expected * factor + bias)
            source = self.source(f"GENE main i32/2\nMOV r3 r0\nMOV r4 r1\nLOAD r5 1\n"
                                 f"LOAD r6 {threshold}\nLABEL loop _\nJZ r3 done\nMOV r2 r3\n"
                                 "LT r2 r6\nJNZ r2 small\nCALL scale r4\nMOV r4 r0\nJMP next _\n"
                                 "LABEL small _\nADD r4 r3\nLABEL next _\nSUB r3 r5\nJMP loop _\n"
                                 "LABEL done _\nRET r4 _\nEND _ _\nGENE scale i32/1\n"
                                 f"LOAD r1 {factor}\nMUL r0 r1\nLOAD r1 {bias}\nADD r0 r1\nRET r0 _\nEND _ _\n")
            for command in ("run", "interpret"):
                with self.subTest(case=case, command=command):
                    self.assertEqual(int(self.call(command, source, count, seed).stdout), expected)

    def test_run_without_toolchain_on_path(self):
        source = self.source("GENE main i32\nLOAD r0 42\nRET r0 _\nEND _ _\n")
        env = dict(os.environ, PATH=str(Path(os.environ["SystemRoot"]) / "System32"))
        self.assertEqual(self.call("run", source, env=env).stdout.strip(), "42")

    def test_csv_shipped_examples_and_all_arities(self):
        for name, total in [("main", 1), ("sum", 4), ("price", 3), ("factorial", 5)]:
            with self.subTest(name=name):
                proc = self.call("test", EXAMPLES / f"{name}.dna", EXAMPLES / f"{name}.csv", "--json")
                data = json.loads(proc.stdout)
                self.assertEqual(proc.stderr, "")
                self.assertTrue(data["ok"])
                self.assertEqual(data["summary"], {"total": total, "passed": total, "failed": 0,
                                                  "mismatches": 0, "errors": 0})
                self.assertTrue(all(case["status"] == "passed" for case in data["cases"]))
        text = self.call("test", EXAMPLES / "sum.dna", EXAMPLES / "sum.csv").stdout
        self.assertIn("Tests: 4 total, 4 passed, 0 failed", text)
        self.assertIn("inputs=[10] expected=55 actual=55", text)

    def test_arithmetic_boundaries_both_engines(self):
        values = [-2147483648, -7, -1, 0, 1, 2, 7, 2147483647]
        ops = ["DIV", "MOD", "AND", "OR", "XOR", "NOT", "LAND", "LOR", "LXOR", "LNOT", "SHL", "SHR", "SAR"]
        for op in ops:
            unary = op in ("NOT", "LNOT")
            source = self.source(f"GENE main i32/{1 if unary else 2}\n{op} r0 {'_' if unary else 'r1'}\nRET r0 _\nEND _ _\n")
            right = [0] if unary else [-2147483648, -1, 0, 1, 2, 15, 30, 31, 32, 2147483647] if op in ("SHL", "SHR", "SAR") else values
            for a in values:
                for b in right:
                    try:
                        expected, error = arithmetic_reference(op, a, b), None
                    except ValueError as exc:
                        expected, error = None, str(exc)
                    messages = []
                    for command in ("run", "interpret"):
                        with self.subTest(op=op, a=a, b=b, command=command):
                            data = json.loads(self.call(command, source, *([a] if unary else [a, b]),
                                                        "--json", code=1 if error else 0).stdout)
                            if error:
                                self.assertFalse(data["ok"])
                                self.assertNotIn("result", data)
                                self.assertEqual(data["error"]["stage"], "run")
                                self.assertIn(error, data["error"]["message"])
                                self.assertIn(f"{source}:2:", data["error"]["message"])
                                messages.append(data["error"]["message"])
                            else:
                                self.assertEqual(data["result"], i32(expected))
                    if error:
                        self.assertEqual(messages[0], messages[1])

    def test_arithmetic_generated_mixed_programs_and_dump(self):
        rng = random.Random(505)
        ops = ["DIV", "MOD", "AND", "OR", "XOR", "NOT", "LAND", "LOR", "LXOR", "LNOT", "SHL", "SHR", "SAR"]
        for case in range(100):
            registers = [rng.randrange(-2**31, 2**31) for _ in range(8)]
            lines = ["GENE main i32"] + [f"LOAD r{i} {value}" for i, value in enumerate(registers)]
            for _ in range(30):
                op = rng.choice(ops)
                dst, src = rng.sample(range(8), 2)
                if op in ("SHL", "SHR", "SAR"):
                    registers[src] = rng.randrange(32)
                    lines.append(f"LOAD r{src} {registers[src]}")
                if op in ("DIV", "MOD") and (registers[src] == 0 or (registers[dst] == -2**31 and registers[src] == -1)):
                    registers[src] = rng.randrange(1, 100)
                    lines.append(f"LOAD r{src} {registers[src]}")
                lines.append(f"{op} r{dst} {'_' if op in ('NOT', 'LNOT') else f'r{src}'}")
                registers[dst] = i32(arithmetic_reference(op, registers[dst], registers[src]))
            result_register = rng.randrange(8)
            source = self.source("\n".join(lines + [f"RET r{result_register} _", "END _ _"]))
            for command in ("run", "interpret"):
                with self.subTest(case=case, command=command):
                    self.assertEqual(int(self.call(command, source).stdout), registers[result_register])
            if case < 10:
                normalized = self.source(self.call("dump", source).stdout, "normalized.dna")
                self.assertEqual(int(self.call("run", normalized).stdout), registers[result_register])

    def test_arithmetic_nested_calls_preserve_registers_and_error_location(self):
        helper = self.source("GENE helper i32/2\nDIV r0 r1\nSHL r0 r1\nNOT r0 _\nRET r0 _\nEND _ _\n", "операции.dna")
        source = self.source("IMPORT операции.dna _\nGENE main i32/2\nLOAD r7 19\nCALL helper r0,r1\n"
                             "ADD r0 r1\nADD r0 r7\nRET r0 _\nEND _ _\n")
        for command in ("run", "interpret"):
            self.assertEqual(int(self.call(command, source, 8, 2).stdout), 4)
            data = json.loads(self.call(command, source, 8, 0, "--json", code=1).stdout)
            self.assertIn(f"{helper}:2:", data["error"]["message"])
        updated = self.source("IMPORT операции.dna _\nGENE main i32/2\nMOD r0 r1\nRET r0 _\nEND _ _\n", "updated.dna")
        proc = self.call("live", source, input=f"run 8 0\nrun 8 2\nreload {updated}\nrun 8 3\nquit\n")
        self.assertIn("division by zero", proc.stderr)
        self.assertEqual([line for line in proc.stdout.splitlines() if line.startswith("result=")], ["result=4", "result=2"])

    def test_arithmetic_csv_continues_after_faults(self):
        source = self.source("GENE main i32/2\nDIV r0 r1\nRET r0 _\nEND _ _\n")
        cases = self.source("arg1,arg2,expected\n12,3,4\n1,0,0\n-2147483648,-1,0\n-7,3,-2\n5,2,3\n", "cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["summary"], {"total": 5, "passed": 2, "failed": 3, "mismatches": 1, "errors": 2})
        self.assertEqual([case["status"] for case in data["cases"]], ["passed", "error", "error", "passed", "failed"])
        self.assertNotIn("actual", data["cases"][1])
        self.assertNotIn("actual", data["cases"][2])

    def test_arithmetic_control_flow_and_large_emission(self):
        # A skipped bad operation must not fault; LAND still requires both registers initialized.
        source = self.source("GENE main i32/1\nJZ r0 done\nLOAD r1 0\nDIV r0 r1\nLABEL done _\nRET r0 _\nEND _ _\n")
        for command in ("run", "interpret"):
            self.assertEqual(int(self.call(command, source, 0).stdout), 0)
        bad = self.source("GENE main i32\nLOAD r0 0\nLAND r0 r1\nRET r0 _\nEND _ _\n", "bad.dna")
        self.assertIn("uninitialized register", self.call("check", bad, code=1).stderr)
        missing = self.source("GENE main i32/1\nNOT r0\nRET r0 _\nEND _ _\n", "missing.dna")
        data = json.loads(self.call("run", missing, 0, "--repair", "--json").stdout)
        self.assertEqual(data["result"], -1)
        self.assertEqual(data["stats"]["repairs"], 1)
        source = self.source("GENE main i32/1\nLOAD r1 1\n" + "DIV r0 r1\n" * 2000 + "RET r0 _\nEND _ _\n")
        for command in ("run", "interpret"):
            self.assertEqual(int(self.call(command, source, -2147483648).stdout), -2147483648)

    def test_arithmetic_shipped_examples(self):
        for name, total in [("divide", 7), ("remainder", 6), ("bits", 7), ("flags", 6), ("logic", 5)]:
            with self.subTest(name=name):
                data = json.loads(self.call("test", EXAMPLES / f"{name}.dna", EXAMPLES / f"{name}.csv", "--json").stdout)
                self.assertTrue(data["ok"])
                self.assertEqual(data["summary"]["passed"], total)
        self.assertEqual(int(self.call("interpret", EXAMPLES / "bits.dna", -1).stdout), 32)

    def test_memory_generated_programs_and_dump(self):
        rng = random.Random(606)
        for case in range(70):
            arrays = {"left": [0] * rng.randrange(1, 33), "right": [0] * rng.randrange(1, 33)}
            variables = {"x": rng.randrange(-2**31, 2**31), "y": 0}
            registers = [rng.randrange(-2**31, 2**31) for _ in range(8)]
            lines = ["GENE main i32"]
            lines += [f"VAR {name} {value}" for name, value in variables.items()]
            lines += [f"ARRAY {name} {len(values)}" for name, values in arrays.items()]
            lines += [f"LOAD r{i} {value}" for i, value in enumerate(registers)]
            for _ in range(80):
                op = rng.choice(["GET", "SET", "AGET", "ASET", "LEN", "ADD", "XOR"])
                dst, src = rng.randrange(8), rng.randrange(8)
                name = rng.choice(list(variables if op in ("GET", "SET") else arrays))
                if op in ("AGET", "ASET"):
                    registers[src] = rng.randrange(len(arrays[name]))
                    lines.append(f"LOAD r{src} {registers[src]}")
                if op == "GET":
                    lines.append(f"GET r{dst} {name}")
                    registers[dst] = variables[name]
                elif op == "SET":
                    lines.append(f"SET {name} r{dst}")
                    variables[name] = registers[dst]
                elif op == "LEN":
                    lines.append(f"LEN r{dst} {name}")
                    registers[dst] = len(arrays[name])
                elif op == "AGET":
                    lines.append(f"AGET r{dst} {name},r{src}")
                    registers[dst] = arrays[name][registers[src]]
                elif op == "ASET":
                    lines.append(f"ASET {name} r{src},r{dst}")
                    arrays[name][registers[src]] = registers[dst]
                else:
                    lines.append(f"{op} r{dst} r{src}")
                    registers[dst] = i32(registers[dst] + registers[src] if op == "ADD" else registers[dst] ^ registers[src])
            # Include every final cell and scalar in the checksum, not just the last register.
            lines.append("LOAD r0 0")
            expected = i32(sum(variables.values()) + sum(sum(a) for a in arrays.values()))
            for name in variables:
                lines += [f"GET r1 {name}", "ADD r0 r1"]
            for name, values in arrays.items():
                for index in range(len(values)):
                    lines += [f"LOAD r2 {index}", f"AGET r1 {name},r2", "ADD r0 r1"]
            source = self.source("\n".join(lines + ["RET r0 _", "END _ _"]))
            for command in ("run", "interpret"):
                with self.subTest(case=case, command=command):
                    self.assertEqual(int(self.call(command, source).stdout), expected)
            if case < 10:
                normalized = self.source(self.call("dump", source).stdout, "normalized.dna")
                for command in ("run", "interpret"):
                    self.assertEqual(int(self.call(command, normalized).stdout), expected)

    def test_memory_generated_loops_with_calls(self):
        rng = random.Random(607)
        for case in range(50):
            length, factor = rng.randrange(1, 65), rng.randrange(-10, 11)
            start, step = rng.randrange(-2**31, 2**31), rng.randrange(-200, 201)
            expected = i32(sum(i32(i32(start + i * step) * factor + i) for i in range(length)))
            source = self.source(f"GENE main i32/2\nARRAY data {length}\nVAR sum 0\n"
                                 "MOV r6 r0\nLOAD r2 0\nLOAD r3 1\nLEN r4 data\nLABEL fill _\n"
                                 "CALL transform r6,r2\nASET data r2,r0\nADD r6 r1\nADD r2 r3\n"
                                 "MOV r5 r2\nLT r5 r4\nJNZ r5 fill\nLABEL read _\nSUB r2 r3\n"
                                 "AGET r5 data,r2\nGET r7 sum\nADD r7 r5\nSET sum r7\nJNZ r2 read\n"
                                 "GET r0 sum\nRET r0 _\nEND _ _\nGENE transform i32/2\nARRAY data 1\n"
                                 f"VAR sum {factor}\nLOAD r2 0\nAGET r3 data,r2\nADD r0 r3\n"
                                 "GET r3 sum\nMUL r0 r3\nADD r0 r1\nASET data r2,r0\n"
                                 "SET sum r0\nRET r0 _\nEND _ _\n")
            for command in ("run", "interpret"):
                with self.subTest(case=case, command=command):
                    self.assertEqual(int(self.call(command, source, start, step).stdout), expected)

    def test_memory_bounds_csv_and_live_recovery(self):
        source = self.source("GENE main i32/2\nARRAY data 3\nAGET r2 data,r0\nADD r1 r2\n"
                             "ASET data r0,r1\nAGET r0 data,r0\nRET r0 _\nEND _ _\n", "массив.dna")
        cases = self.source("arg1,arg2,expected\n2,42,42\n-1,7,0\n3,7,0\n2,8,8\n0,-9,-9\n", "cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["summary"], {"total": 5, "passed": 3, "failed": 2, "mismatches": 0, "errors": 2})
        self.assertEqual([case["status"] for case in data["cases"]], ["passed", "error", "error", "passed", "passed"])
        self.assertNotIn("actual", data["cases"][1])
        for index in [-2147483648, -1, 3, 2147483647]:
            messages = []
            for command in ("run", "interpret"):
                result = json.loads(self.call(command, source, index, 8, "--json", code=1).stdout)
                message = result["error"]["message"]
                self.assertIn(f"{source}:3:", message)
                self.assertIn(f"array index {index} out of bounds", message)
                self.assertIn("0..2 (length 3)", message)
                messages.append(message)
            self.assertEqual(*messages)
        updated = self.source("GENE main i32/2\nVAR offset 100\nARRAY data 4\nASET data r0,r1\n"
                              "AGET r0 data,r0\nGET r1 offset\nADD r0 r1\nRET r0 _\nEND _ _\n", "updated.dna")
        proc = self.call("live", source, input=f"run -1 7\nrun 2 8\nreload {updated}\nrun 3 8\nrun 4 8\nrun 0 9\nquit\n")
        self.assertEqual([x for x in proc.stdout.splitlines() if x.startswith("result=")],
                         ["result=8", "result=108", "result=109"])
        self.assertIn("array index -1 out of bounds", proc.stderr)
        self.assertIn("array index 4 out of bounds", proc.stderr)

    def test_memory_static_steps_and_pruned_storage(self):
        source = self.source("GENE main i32\nVAR x -1\nARRAY a 3\nGET r0 x\nLEN r1 a\nADD r0 r1\nRET r0 _\nEND _ _\n")
        data = json.loads(self.call("check", source, "--json").stdout)
        self.assertEqual(data["stats"]["execution_steps"], 8)
        # Uncalled storage chains do not enlarge main's allocation or initialization budget.
        lines = ["GENE main i32", "VAR x 42", "GET r0 x", "RET r0 _", "END _ _"]
        for i in range(6):
            lines += [f"GENE f{i} i32", "ARRAY a 65536", f"CALL f{i+1} _" if i < 5 else "LEN r0 a", "RET r0 _", "END _ _"]
        source = self.source("\n".join(lines))
        for command in ("run", "interpret"):
            data = json.loads(self.call(command, source, "--json").stdout)
            self.assertEqual(data["result"], 42)
            self.assertEqual(data["stats"]["execution_steps"], 3)

    def test_memory_shipped_examples(self):
        for name, total in [("variables", 5), ("array_sum", 6), ("array_at", 8)]:
            with self.subTest(name=name):
                data = json.loads(self.call("test", EXAMPLES / f"{name}.dna", EXAMPLES / f"{name}.csv", "--json").stdout)
                self.assertEqual(data["summary"]["passed"], total)
        for command in ("run", "interpret"):
            self.assertEqual(int(self.call(command, EXAMPLES / "array_sum.dna", 2, 3).stdout), 392)
            self.assertIn("array index 8 out of bounds", self.call(command, EXAMPLES / "array_at.dna", 8, code=1).stderr)

    def test_list_roundtrip_unicode_encodings_and_empty(self):
        source = self.source("GENE main i32/2\nARRAY a 8\nREAD a r7\nWRITE a r7\nADD r0 r1\nRET r0 _\nEND _ _\n")
        inputs = [b"", b"\xef\xbb\xbf", b"\n \t\r\n", b"-2147483648\n0\n2147483647",
                  b"\xef\xbb\xbf  +7 \r\n\r\n\t-0\t\r\n00012\r\n"]
        for index, content in enumerate(inputs):
            path = self.root / "Список чисел.txt"
            path.write_bytes(content)
            expected = [int(line.strip()) for line in content.decode("utf-8-sig").splitlines() if line.strip()]
            for command in ("run", "interpret"):
                output = self.root / f"Результат {index} {command}.txt"
                proc = self.call(command, source, 12, "--input", path, "--output", output, -5, "--json")
                data = json.loads(proc.stdout)
                self.assertEqual(data["result"], 7)
                self.assertEqual(data["output_count"], len(expected))
                self.assertEqual(data["output"], str(output))
                self.assertEqual(output.read_bytes(), "".join(f"{n}\n" for n in expected).encode())
                self.assertEqual(proc.stderr, "")
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_list_input_errors_have_locations(self):
        source = self.source("GENE main i32\nARRAY a 8\nREAD a r0\nWRITE a r0\nRET r0 _\nEND _ _\n")
        cases = [(b"7\nno\n", "decimal i32", 2), (b"2147483648\n", "decimal i32", 1),
                 (b"-2147483649\n", "decimal i32", 1), (b"1,2\n", "decimal i32", 1),
                 (b"1 2\n", "decimal i32", 1), (b"0x10\n", "decimal i32", 1),
                 (b"1.5\n", "decimal i32", 1), (b"# comment\n", "decimal i32", 1),
                 (b"7\n\xff\n", "valid UTF-8", 2), (b"7\x00\n", "NUL", 1),
                 (b"0" * 512 + b"\n", "511 bytes", 1), (b"\n\xef\xbb\xbf7\n", "decimal i32", 2)]
        path, output = self.root / "input.txt", self.root / "output.txt"
        for content, message, line in cases:
            path.write_bytes(content)
            for command in ("run", "interpret"):
                with self.subTest(content=content[:20], command=command):
                    data = json.loads(self.call(command, source, "--input", path, "--output", output, "--json", code=1).stdout)
                    self.assertEqual(data["error"]["stage"], "file")
                    self.assertIn(f"{path}:{line}:", data["error"]["message"])
                    self.assertIn(message, data["error"]["message"])
                    self.assertFalse(output.exists())
        path.write_bytes(b"0" * 511 + b"\n")
        self.assertEqual(self.call("run", source, "--input", path, "--output", output).stdout.strip(), "1")
        self.assertEqual(output.read_text(), "0\n")

    def test_list_bounds_channels_and_failure_discards_output(self):
        source = self.source("GENE main i32/1\nARRAY a 3\nREAD a r2\nWRITE a r0\n"
                             "LOAD r1 0\nDIV r0 r1\nRET r0 _\nEND _ _\n")
        path = self.source("7\n8\n9\n", "input.txt")
        output = self.root / "output.txt"
        for command in ("run", "interpret"):
            for count in [-2147483648, -1, 4, 2147483647]:
                data = json.loads(self.call(command, source, count, "--input", path, "--output", output, "--json", code=1).stdout)
                self.assertEqual(data["error"]["stage"], "run")
                self.assertIn(f"WRITE count {count} out of bounds: expected 0..3", data["error"]["message"])
                self.assertFalse(output.exists())
            self.assertIn("division by zero", self.call(command, source, 3, "--input", path, "--output", output, code=1).stderr)
            self.assertFalse(output.exists())
            self.assertIn("READ requires an input list", self.call(command, source, 3, "--output", output, code=1).stderr)
            self.assertIn("WRITE requires an output list", self.call(command, source, 3, "--input", path, code=1).stderr)
        path.write_text("1\n2\n3\n4\n")
        for command in ("run", "interpret"):
            self.assertIn("READ input has 4 numbers; array capacity is 3",
                          self.call(command, source, 3, "--input", path, "--output", output, code=1).stderr)
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_list_output_preserves_existing_files_and_no_write(self):
        source = self.source("GENE main i32\nARRAY a 3\nREAD a r0\nWRITE a r0\nRET r0 _\nEND _ _\n")
        path = self.source("1\n2\n3\n", "input.txt")
        existing = self.source("do not change", "existing.txt")
        for command in ("run", "interpret"):
            for output in [existing, path, source]:
                before = output.read_bytes()
                data = json.loads(self.call(command, source, "--input", path, "--output", output, "--json", code=1).stdout)
                self.assertEqual(data["error"]["stage"], "file")
                self.assertIn("already exists", data["error"]["message"])
                self.assertEqual(output.read_bytes(), before)
            self.call(command, source, "--input", path, "--output", self.root / "absent" / "out.txt", code=1)
            self.call(command, source, "--input", path, "--output", str(existing) + ":stream", code=1)
            self.assertEqual(existing.read_text(), "do not change")
        without = self.source("GENE main i32\nLOAD r0 42\nRET r0 _\nEND _ _\n", "no-write.dna")
        output = self.root / "output.txt"
        self.assertIn("did not execute WRITE", self.call("run", without, "--output", output, code=1).stderr)
        self.assertFalse(output.exists())
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_list_options_and_file_failures(self):
        source = self.source("GENE main i32\nARRAY a 1\nREAD a r0\nRET r0 _\nEND _ _\n")
        for flags in [("--input",), ("--output",), ("--input", "--json"),
                      ("--input", "a", "--input", "b"), ("--output", "a", "--output", "b")]:
            data = json.loads(self.call("run", source, *flags, "--json", code=2).stdout)
            self.assertEqual(data["error"]["stage"], "arguments")
        for command in ["check", "dump", "live", "new"]:
            self.call(command, source, "--input", "a", code=2)
        self.call("test", source, "a.csv", "--output", "b", code=2)
        self.call("emit", source, "a.bin", "--input", "b", code=2)
        for path in [self.root / "absent.txt", self.root, "NUL"]:
            data = json.loads(self.call("run", source, "--input", path, "--json", code=1).stdout)
            self.assertEqual(data["error"]["stage"], "file")

    def test_list_limits_and_transfer_budget(self):
        source = self.source("GENE main i32\nARRAY a 65536\nREAD a r0\nWRITE a r0\nRET r0 _\nEND _ _\n")
        path = self.root / "input.txt"
        path.write_bytes(b"-2147483648\n" * 65536)
        for command in ("run", "interpret"):
            output = self.root / f"{command}.txt"
            self.assertEqual(self.call(command, source, "--input", path, "--output", output).stdout.strip(), "65536")
            self.assertEqual(output.read_bytes(), path.read_bytes())
        for content, message in [(b"0\n" * 65537, "65536 numbers"), (b"\n" * (1024 * 1024 + 1), "1 MiB")]:
            path.write_bytes(content)
            self.assertIn(message, self.call("run", source, "--input", path, code=1).stderr)
        path.write_bytes(b"0\n" * 65536)
        source = self.source("GENE main i32/1\nARRAY a 65536\nREAD a r1\nLOAD r2 1\nLABEL loop _\n"
                             "WRITE a r1\nSUB r0 r2\nJNZ r0 loop\nRET r1 _\nEND _ _\n")
        for command in ("run", "interpret"):
            output = self.root / f"budget-{command}.txt"
            self.assertIn("execution budget exhausted", self.call(command, source, 14, "--input", path, "--output", output, code=1).stderr)
            self.assertFalse(output.exists())
            self.assertEqual(self.call(command, source, 13, "--input", path, "--output", output).stdout.strip(), "65536")

    def test_list_snapshots_repeated_read_write_and_dump(self):
        source = self.source("GENE main i32\nVAR pad 99\nARRAY a 4\nREAD a r0\n"
                             "LOAD r1 0\nLOAD r2 77\nASET a r1,r2\nWRITE a r0\n"
                             "READ a r7\nLOAD r6 1\nWRITE a r6\nASET a r1,r2\nRET r7 _\nEND _ _\n")
        normalized = self.source(self.call("dump", source).stdout, "normalized.dna")
        path = self.source("-7\n8\n9\n", "input.txt")
        for i, program in enumerate([source, normalized]):
            for command in ("run", "interpret"):
                output = self.root / f"{command}-{i}.txt"
                self.assertEqual(self.call(command, program, "--input", path, "--output", output).stdout.strip(), "3")
                self.assertEqual(output.read_text(), "-7\n")
        self.assertIsNone(json.loads(self.call("check", source, "--json").stdout)["stats"]["execution_steps"])
        # A skipped READ/WRITE does not require a file channel.
        skipped = self.source("GENE main i32\nARRAY a 1\nJMP done _\nREAD a r0\nWRITE a r0\n"
                              "LABEL done _\nLOAD r0 42\nRET r0 _\nEND _ _\n")
        for command in ("run", "interpret"):
            self.assertEqual(self.call(command, skipped).stdout.strip(), "42")

    def test_list_generated_practical_programs(self):
        rng = random.Random(707)
        datasets = [[], [5], [7, -3, 0, 7, 12, -9, 4], [2147483647, -2147483648, 0], list(range(255, -1, -1))]
        datasets += [[rng.randrange(-2**31, 2**31) for _ in range(rng.randrange(81))] for _ in range(40)]
        for case, values in enumerate(datasets):
            path = self.source("".join(f"{n}\n" for n in values), "input.txt")
            threshold = rng.randrange(-2**31, 2**31)
            for command in ("run", "interpret"):
                with self.subTest(case=case, command=command):
                    self.assertEqual(int(self.call(command, EXAMPLES / "list_sum.dna", "--input", path).stdout), i32(sum(values)))
                    for name, args, expected in [("list_sort", [], sorted(values)),
                                                  ("list_filter", [threshold], [n for n in values if n >= threshold])]:
                        output = self.root / f"{case}-{command}-{name}.txt"
                        proc = self.call(command, EXAMPLES / f"{name}.dna", *args, "--input", path, "--output", output)
                        self.assertEqual(int(proc.stdout), len(expected))
                        self.assertEqual([int(n) for n in output.read_text().splitlines()], expected)

    def test_list_competing_writers_publish_one_complete_file(self):
        source = self.source("GENE main i32\nARRAY a 65536\nREAD a r0\nWRITE a r0\nRET r0 _\nEND _ _\n")
        inputs = [self.source((str(value) + "\n") * 65536, f"input-{value}.txt") for value in [7, -9]]
        output = self.root / "result.txt"
        processes = [subprocess.Popen([str(EXE), "run", str(source), "--input", str(path), "--output", str(output)],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE, encoding="utf-8") for path in inputs]
        results = [p.communicate(timeout=10) for p in processes]
        self.assertEqual(sorted(p.returncode for p in processes), [0, 1], results)
        winner = next(i for i, p in enumerate(processes) if p.returncode == 0)
        # Windows text fixtures use CRLF; exported lists deliberately use LF.
        self.assertEqual(output.read_bytes(), inputs[winner].read_text(encoding="utf-8").encode("utf-8"))
        self.assertEqual(list(self.root.glob("*.tmp")), [])

    def test_csv_lists_shipped_suite_without_file_changes(self):
        watched = [EXAMPLES / "list_pipeline.csv", EXAMPLES / "numbers.txt", EXAMPLES / "numbers_filtered.txt"]
        watched += list((EXAMPLES / "list_cases").glob("*.txt"))
        before = {path: path.read_bytes() for path in watched}
        data = json.loads(self.call("test", EXAMPLES / "list_pipeline.dna", EXAMPLES / "list_pipeline.csv", "--json").stdout)
        self.assertEqual(data["summary"], {"total": 8, "passed": 8, "failed": 0, "mismatches": 0, "errors": 0})
        self.assertTrue(all(case["output"]["matched"] for case in data["cases"]))
        text = self.call("test", EXAMPLES / "list_pipeline.dna", EXAMPLES / "list_pipeline.csv").stdout
        self.assertIn("8 total, 8 passed, 0 failed", text)
        self.assertEqual(before, {path: path.read_bytes() for path in watched})
        self.assertEqual(list(self.root.iterdir()), [])

    def test_csv_lists_all_arities_unicode_paths_and_formats(self):
        input_path = self.source("\ufeff\n -2147483648 \n+7\n2147483647\n", "Данные с пробелами/вход,числа.txt")
        expected = self.source("-2147483648\r\n7\r\n2147483647\r\n", "Данные с пробелами/эталон.txt")
        for arity in range(3):
            source = self.source(f"GENE main i32/{arity}\nARRAY a 3\nREAD a r2\nWRITE a r2\n"
                                 + ("LOAD r0 3\n" if arity == 0 else "" if arity == 1 else "ADD r0 r1\n")
                                 + "RET r0 _\nEND _ _\n")
            cells = ["arg1", "arg2"][:arity] + ["input", "expected", "expected_output"]
            args = [3] if arity == 1 else [1, 2] if arity == 2 else []
            text = io.StringIO(newline="")
            writer = csv.writer(text, quoting=csv.QUOTE_ALL, lineterminator="\r\n")
            writer.writerow(cells)
            # Relative path from CSV, with a comma; absolute expected path also works.
            writer.writerow(args + ["../Данные с пробелами/вход,числа.txt", 3, str(expected)])
            cases = self.source("", f"Наборы/arity-{arity}.csv")
            cases.write_bytes(("\ufeff\r\n" + text.getvalue().rstrip("\r\n")).encode("utf-8"))
            data = json.loads(self.call("test", source, cases, "--json").stdout)
            case = data["cases"][0]
            self.assertEqual(case["line"], 3)
            self.assertEqual(case["inputs"], args)
            self.assertEqual(case["output"], {"matched": True, "expected_count": 3, "actual_count": 3})
        self.assertTrue(input_path.is_file())

    def test_csv_lists_first_difference_length_order_and_scalar(self):
        source = self.source("GENE main i32\nARRAY a 8\nREAD a r0\nWRITE a r0\nRET r0 _\nEND _ _\n")
        self.source("-2\n7\n9\n", "input.txt")
        expected_lists = [[-2, 8, 9], [-2, 7], [-2, 7, 9, 11], [], [9, 7, -2], [-1], [-2, 7, 9]]
        rows = ["input,expected,expected_output"]
        for i, values in enumerate(expected_lists):
            self.source("".join(f"{n}\n" for n in values), f"expected-{i}.txt")
            rows.append(f"input.txt,3,expected-{i}.txt")
        rows += ["input.txt,4,input.txt", "input.txt,3,input.txt"]
        cases = self.source("\n".join(rows), "cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["summary"], {"total": 9, "passed": 2, "failed": 7, "mismatches": 7, "errors": 0})
        differences = [(2, 8, 7), (3, None, 9), (4, 11, None), (1, None, -2), (1, 9, -2), (1, -1, -2)]
        for case, (element, expected, actual) in zip(data["cases"], differences):
            self.assertEqual(case["actual"], 3)  # Scalar result alone would have passed.
            self.assertEqual(case["output"]["difference"], {"element": element, "expected": expected, "actual": actual})
        self.assertTrue(data["cases"][-2]["output"]["matched"])
        self.assertEqual(data["cases"][-2]["status"], "failed")
        text = self.call("test", source, cases, code=1).stdout
        self.assertIn("element 2: expected=8 actual=7", text)
        self.assertIn("element 3: expected=<missing> actual=9", text)
        self.assertIn("element 4: expected=11 actual=<missing>", text)
        self.assertIn("output_count: expected=2 actual=3", text)

    def test_csv_lists_file_errors_continue_and_identify_fixture(self):
        source = self.source("GENE main i32\nARRAY a 8\nREAD a r0\nWRITE a r0\nRET r0 _\nEND _ _\n")
        good = self.source("7\n", "данные/good.txt")
        bad = self.source("7\nnot-a-number\n", "данные/bad.txt")
        cases = self.source("input,expected,expected_output\n../данные/missing.txt,1,../данные/good.txt\n"
                            "../данные/bad.txt,1,../данные/good.txt\n../данные/good.txt,1,../данные/bad.txt\n"
                            "../данные/good.txt,1,../данные/missing.txt\n../данные/good.txt,1,../данные/good.txt\n", "наборы/cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["summary"]["errors"], 4)
        self.assertEqual(data["summary"]["passed"], 1)
        for i, field in enumerate(["input", "input", "expected_output", "expected_output"]):
            case = data["cases"][i]
            self.assertEqual(case["line"], i + 2)
            self.assertEqual(case["error"]["stage"], "file")
            self.assertEqual(case["error"]["field"], field)
            self.assertNotIn("actual", case)
            self.assertNotIn("output", case)
        self.assertEqual(data["cases"][2]["error"]["file"], str(bad))
        self.assertIn(str(bad) + ":2:", data["cases"][2]["error"]["message"])
        self.assertEqual(good.read_text(), "7\n")

    def test_csv_lists_execution_errors_and_fresh_budgets(self):
        source = self.source("GENE main i32/1\nARRAY a 1\nREAD a r1\nWRITE a r1\nLOAD r2 1\n"
                             "JZ r0 done\nLABEL loop _\nSUB r0 r2\nJNZ r0 loop\nLABEL done _\nRET r1 _\nEND _ _\n")
        self.source("7\n", "one.txt")
        self.source("7\n8\n", "two.txt")
        cases = self.source("arg1,input,expected,expected_output\n499996,one.txt,1,one.txt\n"
                            "499997,one.txt,1,one.txt\n0,two.txt,2,two.txt\n499996,one.txt,1,one.txt\n"
                            "0,one.txt,1,one.txt\n", "cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual([case["status"] for case in data["cases"]], ["passed", "error", "error", "passed", "passed"])
        self.assertIn("execution budget exhausted", data["cases"][1]["error"]["message"])
        self.assertIn("array capacity is 1", data["cases"][2]["error"]["message"])
        self.assertEqual(data["cases"][1]["error"]["stage"], "run")
        self.assertNotIn("output", data["cases"][1])  # A staged WRITE is not a successful result.
        self.assertNotIn("actual", data["cases"][1])

    def test_csv_lists_missing_write_is_not_an_empty_result(self):
        source = self.source("GENE main i32/1\nARRAY a 1\nJZ r0 done\nLOAD r1 0\nWRITE a r1\n"
                             "LABEL done _\nLOAD r0 0\nRET r0 _\nEND _ _\n")
        self.source("", "empty.txt")
        cases = self.source("arg1,input,expected,expected_output\n1,empty.txt,0,empty.txt\n"
                            "0,empty.txt,0,empty.txt\n1,empty.txt,0,empty.txt\n", "cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual([case["status"] for case in data["cases"]], ["passed", "error", "passed"])
        self.assertIn("did not execute WRITE", data["cases"][1]["error"]["message"])
        self.assertEqual(data["cases"][0]["output"]["actual_count"], 0)
        self.assertNotIn("output", data["cases"][1])

    def test_csv_lists_last_write_snapshot_and_fresh_data(self):
        source = self.source("GENE main i32\nARRAY a 3\nREAD a r0\nLOAD r1 1\nWRITE a r1\n"
                             "WRITE a r0\nLOAD r1 0\nLOAD r2 99\nASET a r1,r2\nRET r0 _\nEND _ _\n")
        numbers = self.source("7\n8\n9\n", "numbers.txt")
        cases = self.source("input,expected,expected_output\nnumbers.txt,3,numbers.txt\n"
                            "numbers.txt,3,numbers.txt\n", "cases.csv")
        before = {path: path.read_bytes() for path in self.root.iterdir()}
        self.call("test", source, cases)
        self.assertEqual(before, {path: path.read_bytes() for path in self.root.iterdir()})
        self.assertEqual(numbers.read_text(), "7\n8\n9\n")
        fresh = self.source("GENE main i32\nARRAY a 1\nLOAD r1 0\nAGET r0 a,r1\n"
                            "LOAD r2 9\nASET a r1,r2\nLOAD r1 1\nWRITE a r1\nRET r0 _\nEND _ _\n")
        self.source("9\n", "nine.txt")
        cases = self.source("input,expected,expected_output\nnumbers.txt,0,nine.txt\n"
                            + "numbers.txt,0,nine.txt\n", "cases.csv")
        self.assertEqual(json.loads(self.call("test", fresh, cases, "--json").stdout)["summary"]["passed"], 2)

    def test_csv_lists_bad_csv_is_rejected_before_cases(self):
        source = EXAMPLES / "list_pipeline.dna"
        rows = [("5,,3,expected.txt", "path", 2), ("5,input.txt,3,", "path", 4),
                ('5,"",3,expected.txt', "path", 2), ('5,input.txt,3," "', "path", 4),
                ("5,input.txt,,expected.txt", "integer", 3), ("no,input.txt,3,expected.txt", "integer", 1),
                ("5,input.txt,2147483648,expected.txt", "integer", 3),
                ("5,input.txt,3", "column_count", 4), ("5,input.txt,3,expected.txt,extra", "column_count", 5),
                ('5,"unclosed,3,expected.txt', "quotes", 2), ('5,in"put.txt,3,expected.txt', "quotes", 2)]
        for row, error_code, column in rows:
            with self.subTest(row=row):
                cases = self.source("arg1,input,expected,expected_output\n5,missing.txt,3,also_missing.txt\n" + row, "cases.csv")
                data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
                self.assertEqual(data["error"]["stage"], "csv")
                self.assertEqual(data["error"]["code"], error_code)
                self.assertEqual(data["error"]["line"], 3)
                self.assertEqual(data["error"]["column"], column)
                self.assertNotIn("cases", data)
        for header in ["arg1,expected,input,expected_output", "arg1,input,expected,output", "input,expected,expected_output",
                       "arg1,input,expected", "arg1,arg2,input,expected,expected_output"]:
            cases = self.source(header + "\n", "cases.csv")
            data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
            self.assertEqual(data["error"]["stage"], "csv")
            self.assertIn("arg1,input,expected,expected_output", data["error"]["message"])

    def test_csv_lists_invalid_fixtures_are_not_executed(self):
        # Invalid expected data wins over an infinite program because both files load first.
        source = EXAMPLES / "infinite.dna"
        self.source("7\n", "good.txt")
        bad = self.source("", "bad.txt")
        cases = self.source("input,expected,expected_output\ngood.txt,0,bad.txt\n", "cases.csv")
        for content, message in [(b"\xff\n", "UTF-8"), (b"1\x00\n", "NUL"), (b"1,2\n", "one decimal i32"),
                                 (b"2147483648\n", "one decimal i32"), (b"0" * 512, "511 bytes"),
                                 (b"0\n" * 65537, "65536 numbers"), (b"\n" * (1024 * 1024 + 1), "1 MiB")]:
            with self.subTest(message=message):
                bad.write_bytes(content)
                data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
                error = data["cases"][0]["error"]
                self.assertEqual(error["stage"], "file")
                self.assertEqual(error["field"], "expected_output")
                self.assertIn(message, error["message"])
                self.assertNotIn("execution budget", error["message"])
        # A device must not become a test input; the shared list reader rejects it.
        cases.write_text("input,expected,expected_output\nNUL,0,good.txt\n", encoding="utf-8")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["cases"][0]["error"]["stage"], "file")
        self.assertIn("regular disk file", data["cases"][0]["error"]["message"])

    def test_csv_lists_maximum_list_and_case_counts(self):
        source = self.source("GENE main i32\nARRAY a 65536\nREAD a r0\nWRITE a r0\nRET r0 _\nEND _ _\n")
        self.source("-2147483648\n" * 65536, "large.txt")
        expected = self.source("-2147483648\n" * 65535 + "2147483647\n", "expected.txt")
        cases = self.source("input,expected,expected_output\nlarge.txt,65536,large.txt\n"
                            "large.txt,65536,expected.txt\n", "cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["cases"][0]["status"], "passed")
        self.assertEqual(data["cases"][1]["output"]["difference"],
                         {"element": 65536, "expected": 2147483647, "actual": -2147483648})
        # Case buffers are reused/freed; a maximum suite does not retain 1000 pairs of lists.
        source = self.source("GENE main i32\nARRAY a 1\nLOAD r0 0\nWRITE a r0\nRET r0 _\nEND _ _\n")
        self.source("", "empty.txt")
        cases.write_text("input,expected,expected_output\n" + "empty.txt,0,empty.txt\n" * 1000, encoding="utf-8")
        self.assertEqual(json.loads(self.call("test", source, cases, "--json").stdout)["summary"]["passed"], 1000)
        with cases.open("a", encoding="utf-8") as file:
            file.write("empty.txt,0,empty.txt\n")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["error"]["code"], "case_limit")
        self.assertNotIn("cases", data)
        self.assertTrue(expected.is_file())

    def test_csv_lists_generated_pipeline_references(self):
        rng = random.Random(909)
        rows = ["arg1,input,expected,expected_output"]
        for i in range(50):
            values = [rng.randint(-2147483648, 2147483647) for _ in range(rng.randrange(129))]
            threshold = rng.randint(-2147483648, 2147483647)
            expected = sorted(v for v in values if v >= threshold)
            self.source("".join(f"{v}\n" for v in values), f"lists/in-{i}.txt")
            self.source("".join(f"{v}\n" for v in expected), f"lists/out-{i}.txt")
            rows.append(f"{threshold},lists/in-{i}.txt,{len(expected)},lists/out-{i}.txt")
        cases = self.source("\n".join(rows), "cases.csv")
        env = dict(os.environ, PATH=str(Path(os.environ["SystemRoot"]) / "System32"))
        data = json.loads(self.call("test", EXAMPLES / "list_pipeline.dna", cases, "--json", env=env).stdout)
        self.assertEqual(data["summary"]["passed"], 50)
        self.assertTrue(all(case["output"]["matched"] for case in data["cases"]))

    def test_csv_mismatches_do_not_stop_the_suite(self):
        source = self.source("GENE main i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n")
        cases = self.source("arg1,arg2,expected\n10,20,31\n-7,4,-3\n1,2,4\n", "cases.csv")
        proc = self.call("test", source, cases, "--json", code=1)
        data = json.loads(proc.stdout)
        self.assertFalse(data["ok"])
        self.assertEqual(proc.stderr, "")
        self.assertEqual(data["summary"], {"total": 3, "passed": 1, "failed": 2, "mismatches": 2, "errors": 0})
        self.assertEqual(data["cases"][0], {"line": 2, "inputs": [10, 20], "expected": 31,
                                            "actual": 30, "status": "failed"})
        self.assertEqual([case["status"] for case in data["cases"]], ["failed", "passed", "failed"])
        text = self.call("test", source, cases, code=1).stdout
        self.assertIn(f"FAIL {cases}:2: inputs=[10,20] expected=31 actual=30", text)
        self.assertIn(f"PASS {cases}:3:", text)
        self.assertIn("3 total, 1 passed, 2 failed", text)

    def test_csv_runtime_errors_continue_with_a_fresh_budget(self):
        source = self.source("GENE main i32/1\nJZ r0 done\nLABEL spin _\nJMP spin _\n"
                             "LABEL done _\nLOAD r0 7\nRET r0 _\nEND _ _\n")
        cases = self.source("arg1,expected\n1,0\n0,7\n0,8\n1,0\n0,7\n", "cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["summary"], {"total": 5, "passed": 2, "failed": 3, "mismatches": 1, "errors": 2})
        first = data["cases"][0]
        self.assertEqual(first["status"], "error")
        self.assertNotIn("actual", first)
        self.assertEqual(first["error"]["stage"], "run")
        self.assertIn(f"{source}:4:", first["error"]["message"])
        self.assertIn("execution budget exhausted", first["error"]["message"])
        self.assertEqual(data["cases"][-1]["actual"], 7)
        text = self.call("test", source, cases, code=1).stdout
        self.assertIn(f"ERROR {cases}:2:", text)
        self.assertIn("1 mismatches, 2 execution errors", text)

    def test_csv_header_must_match_main(self):
        for arity, header in [(0, "expected"), (1, "arg1,expected"), (2, "arg1,arg2,expected")]:
            source = self.source(f"GENE main i32/{arity}\nLOAD r0 42\nRET r0 _\nEND _ _\n")
            for invalid in ["input,expected", "arg2,arg1,expected", "ARG1,expected", "1,42", "arg1;expected"]:
                with self.subTest(arity=arity, header=invalid):
                    cases = self.source(invalid + "\n", "cases.csv")
                    data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
                    self.assertEqual(data["error"]["stage"], "csv")
                    self.assertEqual(data["error"]["line"], 1)
                    self.assertIn(f"CSV header must be {header}", data["error"]["message"])
                    self.assertNotIn("cases", data)

    def test_csv_empty_files_and_header_only_are_errors(self):
        source = EXAMPLES / "sum.dna"
        for content in ["", "\ufeff", "\n \t\n", "arg1,expected\n", "arg1,expected\n\n"]:
            with self.subTest(content=content):
                cases = self.source(content, "empty.csv")
                data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
                self.assertFalse(data["ok"])
                self.assertIn(data["error"]["code"], ["header", "empty"])
                self.assertEqual(data["error"]["line"], 0)
                self.assertNotIn("summary", data)

    def test_csv_reports_invalid_cells_before_running_any_examples(self):
        bad_rows = [("1,", "integer", 2), (",0", "integer", 1), ("1", "column_count", 2),
                    ("1,1,1", "column_count", 3), ("1,1,", "column_count", 3),
                    ("2147483648,0", "integer", 1), ("0,-2147483649", "integer", 2),
                    ("1.5,1", "integer", 1), ("0x10,1", "integer", 1), ("+,1", "integer", 1),
                    ('"",1', "integer", 1), ('"1,2",0', "integer", 1),
                    ('1"2,0', "quotes", 1), ('"1,0', "quotes", 1),
                    ('"1"junk,0', "quotes", 1), ('"1""2",0', "quotes", 1)]
        for row, error_code, column in bad_rows:
            with self.subTest(row=row):
                cases = self.source("arg1,expected\n1,1\n\n" + row + "\n", "cases.csv")
                proc = self.call("test", EXAMPLES / "sum.dna", cases, "--json", code=1)
                data = json.loads(proc.stdout)
                self.assertEqual(proc.stderr, "")
                self.assertEqual(data["error"]["code"], error_code)
                self.assertEqual(data["error"]["file"], str(cases))
                self.assertEqual(data["error"]["line"], 4)
                self.assertEqual(data["error"]["column"], column)
                self.assertNotIn("cases", data)
        proc = self.call("test", EXAMPLES / "sum.dna", cases, code=1)
        self.assertEqual(proc.stdout, "")
        self.assertIn(f"{cases}:4: column 1:", proc.stderr)

    def test_csv_unicode_bom_quotes_blank_lines_and_newlines(self):
        cases = self.root / "Примеры с пробелами.csv"
        for newline in ["\n", "\r\n"]:
            content = '\ufeff' + newline + ' "arg1" , "expected" ' + newline + newline + ' "10" , "55" ' + newline + '0,0'
            cases.write_bytes(content.encode("utf-8"))
            data = json.loads(self.call("test", EXAMPLES / "sum.dna", cases, "--json").stdout)
            self.assertEqual([case["line"] for case in data["cases"]], [4, 5])
            self.assertEqual(data["summary"]["passed"], 2)
        cases.write_bytes(b"arg1,expected\n" + b"0" * 509 + b",0\n")
        self.assertEqual(json.loads(self.call("test", EXAMPLES / "sum.dna", cases, "--json").stdout)["summary"]["passed"], 1)

    def test_csv_rejects_damaged_encoding_and_nul(self):
        cases = self.root / "bad.csv"
        for content in [b"arg1,expected\n\xff,0\n", b"arg1,expected\n1\x00,1\n",
                        b"arg1,expected\n1,1\n#\xff\n", "arg1,expected\n1,1".encode("utf-16")]:
            cases.write_bytes(content)
            data = json.loads(self.call("test", EXAMPLES / "sum.dna", cases, "--json", code=1).stdout)
            self.assertEqual(data["error"]["code"], "encoding")
            self.assertNotIn("cases", data)

    def test_csv_integer_boundaries_and_standard_writer(self):
        source = self.source("GENE main i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n")
        values = [-2147483648, -1, 0, 1, 2147483647]
        text = io.StringIO(newline="")
        writer = csv.writer(text, quoting=csv.QUOTE_ALL)
        writer.writerow(["arg1", "arg2", "expected"])
        for a in values:
            for b in values:
                writer.writerow([a, b, (a + b + 2**31) % 2**32 - 2**31])
        cases = self.root / "standard.csv"
        cases.write_bytes(text.getvalue().encode("utf-8"))
        data = json.loads(self.call("test", source, cases, "--json").stdout)
        self.assertEqual(data["summary"]["passed"], 25)
        cases.write_text("arg1,arg2,expected\n+1,-0,0001\n", encoding="utf-8")
        self.call("test", source, cases)

    def test_csv_size_and_case_limits(self):
        source = self.source("GENE main i32\nLOAD r0 42\nRET r0 _\nEND _ _\n")
        cases = self.source("expected\n" + "42\n" * 1000, "limit.csv")
        data = json.loads(self.call("test", source, cases, "--json").stdout)
        self.assertEqual(data["summary"]["passed"], 1000)
        for content, error_code in [(b"expected\n" + b"42\n" * 1001, "case_limit"),
                                    (b"expected\n" + b"0" * 512, "line_limit"),
                                    (b"\n" * (1024 * 1024 + 1), "file_limit")]:
            cases.write_bytes(content)
            data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
            self.assertEqual(data["error"]["code"], error_code)
            self.assertNotIn("cases", data)

    def test_csv_argument_file_compile_errors_and_repair(self):
        source = EXAMPLES / "sum.dna"
        for args in [("test", source), ("test", source, "--json"),
                     ("test", source, EXAMPLES / "sum.csv", "extra"),
                     ("test", source, EXAMPLES / "sum.csv", "--interpret")]:
            data = json.loads(self.call(*args, "--json", code=2).stdout)
            self.assertEqual(data["error"]["stage"], "arguments")
        data = json.loads(self.call("test", source, "missing.csv", "--json", code=1).stdout)
        self.assertEqual(data["error"]["code"], "open")
        self.assertEqual(data["error"]["file"], "missing.csv")
        source = self.source("GENE main i32\nLOAD r0 42\nRET r0\nEND _\n")
        cases = self.source("expected\n42\n", "cases.csv")
        data = json.loads(self.call("test", source, cases, "--json", code=1).stdout)
        self.assertEqual(data["error"]["stage"], "compile")
        data = json.loads(self.call("test", source, cases, "--repair", "--json").stdout)
        self.assertTrue(data["ok"])
        env = dict(os.environ, PATH=str(Path(os.environ["SystemRoot"]) / "System32"))
        self.call("test", EXAMPLES / "price.dna", EXAMPLES / "price.csv", env=env)


if __name__ == "__main__":
    unittest.main(verbosity=2)
