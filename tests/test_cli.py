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

    def test_help_and_version(self):
        self.assertIn("Codons 0.6.0", self.call("--version").stdout)
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

    def test_repair_is_explicit(self):
        source = self.source("GENE main i32\nLOAD r0 42\nRET r0\nEND _\n")
        self.call("run", source, code=1)
        data = json.loads(self.call("run", source, "--repair", "--json").stdout)
        self.assertEqual(data["result"], 42)
        self.assertEqual(data["stats"]["repairs"], 2)
        self.assertIn("RET r0\n", source.read_text())

    def test_live_inputs_and_failed_update(self):
        source = self.source("GENE main i32/2\nADD r0 r1\nRET r0 _\nEND _ _\n", "old.dna")
        updated = self.source("GENE main i32/2\nMUL r0 r1\nRET r0 _\nEND _ _\n", "new.dna")
        invalid = self.source("GENE main i32/1\nRET r0 _\nEND _ _\n", "bad.dna")
        proc = self.call("live", source, input=f"run 10 20\nreload {updated}\nrun 10 20\nreload {invalid}\nrun 10 20\nrun\nquit\n")
        self.assertEqual([x for x in proc.stdout.splitlines() if x.startswith("result=")],
                         ["result=30", "result=200", "result=200"])
        self.assertIn("incompatible signature", proc.stderr)
        self.assertIn("expects 2 arguments", proc.stderr)

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
            self.assertIn("execution budget exhausted after 1000000 steps", data["error"]["message"])
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
        self.assertIn("exactly 3 fields", self.call("check", missing, "--repair", code=1).stderr)
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
