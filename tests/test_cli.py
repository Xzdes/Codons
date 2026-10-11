"""Exercise the shipped executable as a user or AI client would. Standard library only."""
import json
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
        self.assertIn("Codons 0.3.0", self.call("--version").stdout)
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


if __name__ == "__main__":
    unittest.main(verbosity=2)
