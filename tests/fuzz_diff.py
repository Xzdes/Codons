"""Differential fuzzer: random .dna programs must behave identically under
`run` (JIT) and `interpret` (reference interpreter). Standard library only.

The generator models definite initialization the way analyze.c does, so most
programs pass validation and actually execute. Some runs end in a planned
runtime fault (division by zero, out of bounds, bad shift, missing I/O); both
backends must report the same fault at the same line. Exit code, stdout and
stderr are compared verbatim.
"""
import argparse
import random
import subprocess
import sys
import tempfile
from pathlib import Path

EXE = Path(sys.argv.pop(1)).resolve()
WORK = Path.cwd() / "work"
WORK.mkdir(exist_ok=True)

I32_POOL = (-2147483648, -100, -7, -1, 0, 1, 2, 5, 31, 32, 100, 2147483647)
BIN_OPS = ("ADD", "SUB", "MUL", "AND", "OR", "XOR", "LAND", "LOR", "LXOR", "EQ", "LT", "MIN", "MAX")
RISKY_OPS = ("DIV", "MOD", "SHL", "SHR", "SAR")
UN_OPS = ("NOT", "LNOT")
FILLABLE_HEADS = ("RET", "END", "NOT", "LNOT", "LABEL", "JMP")
REGS = tuple(f"r{i}" for i in range(8))
STRING_CHARS = "abz AZ019._-#"


class Gene:
    """Generates one statically valid gene. self.init tracks registers that are
    definitely initialized, mirroring the analyzer (intersection at joins)."""

    def __init__(self, rng, name, arity, callees, allow_param):
        self.rng = rng
        self.name = name
        self.arity = arity
        self.callees = callees  # (name, arity, has_param), call graph stays a DAG
        self.lines = []
        self.label_count = 0
        self.vars = []            # (name, initial)
        self.arrays = []          # (name, length); length None marks PARAM
        self.param = None
        if allow_param and arity >= 1 and rng.random() < 0.5:
            self.param = "xs"
            self.arrays.append(("xs", None))
        self.branches = 0
        self.loops = 0
        self.init = {f"r{i}" for i in range(arity)}
        # Registers known to hold a live string handle; best-effort tracking so
        # string ops mostly take their success path (faults would agree too).
        self.strings = set()

    @property
    def has_param(self):
        return self.param is not None

    def local_arrays(self):
        return [a for a in self.arrays if a[1] is not None]

    def emit(self, line):
        self.lines.append(line)

    def new_label(self):
        self.label_count += 1
        return f"L{self.label_count}"

    def dst_reg(self, forbidden=()):
        return self.rng.choice([r for r in REGS if r not in forbidden])

    def init_reg(self):
        return self.rng.choice(sorted(self.init))

    def load(self, reg=None, value=None, forbidden=()):
        reg = reg if reg is not None else self.dst_reg(forbidden)
        if value is None:
            value = self.rng.choice(I32_POOL) if self.rng.random() < 0.4 else self.rng.randint(-100, 100)
        self.emit(f"LOAD {reg} {value}")
        self.init.add(reg)
        self.strings.discard(reg)
        return reg

    def gen_literal(self):
        rng = self.rng
        out = []
        for _ in range(rng.randint(0, 6)):
            pick = rng.random()
            if pick < 0.75:
                out.append(rng.choice(STRING_CHARS))
            elif pick < 0.85:
                out.append(rng.choice(("\\n", "\\t", "\\\\", '\\"')))
            else:
                out.append(f"\\x{rng.randint(0, 255):02x}")
        return '"' + "".join(out) + '"'

    def gen_decls(self):
        for i in range(self.rng.randint(0, 3)):
            if self.rng.random() < 0.5:
                value = self.rng.randint(-100, 100)
                self.vars.append((f"v{i}", value))
                self.emit(f"VAR v{i} {value}")
            else:
                length = self.rng.randint(1, 8)
                self.arrays.append((f"a{i}", length))
                self.emit(f"ARRAY a{i} {length}")
        # An array call needs a local array with a compile-time known length.
        if any(c[2] for c in self.callees) and not self.local_arrays():
            self.arrays.append(("buf", 8))
            self.emit("ARRAY buf 8")

    def gen_branch(self, forbidden):
        cond = self.init_reg()
        else_label, end_label = self.new_label(), self.new_label()
        self.emit(f"{self.rng.choice(('JZ', 'JNZ'))} {cond} {else_label}")
        saved = set(self.init)
        saved_strings = set(self.strings)
        self.branches += 1
        self.gen_statements(self.rng.randint(1, 3), forbidden)
        arm_a = set(self.init)
        strings_a = set(self.strings)
        self.emit(f"JMP {end_label} _")
        self.emit(f"LABEL {else_label} _")
        self.init = set(saved)
        self.strings = set(saved_strings)
        self.gen_statements(self.rng.randint(1, 3), forbidden)
        arm_b = set(self.init)
        strings_b = set(self.strings)
        self.emit(f"LABEL {end_label} _")
        self.init = arm_a & arm_b
        self.strings = strings_a & strings_b

    def gen_loop(self, forbidden):
        counter = self.load(value=self.rng.randint(1, 9), forbidden=forbidden)
        step = self.load(value=1, forbidden=forbidden + (counter,))
        label = self.new_label()
        self.emit(f"LABEL {label} _")
        self.loops += 1
        # The loop head intersects entry and back edge: body definitions are not
        # visible at the label, but remain readable after the fall-through.
        saved_strings = set(self.strings)
        self.gen_statements(self.rng.randint(0, 2), forbidden + (counter, step))
        # A handle survives the body only if the body never overwrote it.
        self.strings &= saved_strings
        self.emit(f"SUB {counter} {step}")
        self.emit(f"JNZ {counter} {label}")

    def gen_param_access(self, forbidden):
        # PARAM length is dynamic: guard index 0 with LEN before touching it.
        length_reg = self.dst_reg(forbidden)
        self.emit(f"LEN {length_reg} {self.param}")
        self.init.add(length_reg)
        self.strings.discard(length_reg)
        skip = self.new_label()
        self.emit(f"JZ {length_reg} {skip}")
        saved = set(self.init)
        saved_strings = set(self.strings)
        index = self.load(value=0, forbidden=forbidden)
        if self.rng.random() < 0.5:
            dst = self.dst_reg(forbidden)
            self.emit(f"AGET {dst} {self.param},{index}")
        else:
            self.emit(f"ASET {self.param} {index},{self.init_reg()}")
        self.emit(f"LABEL {skip} _")
        # The index/dst registers are defined only on the taken path.
        self.init = saved
        self.strings &= saved_strings

    def gen_call(self, forbidden):
        name, arity, has_param = self.rng.choice(self.callees)
        used, parts = [], []
        if has_param:
            array, length = self.rng.choice(self.local_arrays())
            bad = self.rng.random() < 0.15
            length_reg = self.load(value=length + 1 if bad else self.rng.randint(1, length),
                                   forbidden=forbidden)
            used.append(length_reg)
            parts += [array, length_reg]
        # The array name is not an argument; arity counts the length register too.
        while len(parts) - (1 if has_param else 0) < arity:
            reg = self.load(forbidden=forbidden + tuple(used))
            used.append(reg)
            parts.append(reg)
        self.emit(f"CALL {name} {','.join(parts) if parts else '_'}")
        self.init.add("r0")

    def gen_statement(self, forbidden=()):
        rng = self.rng
        kinds, weights = [], []

        def offer(kind, weight, condition=True):
            if condition:
                kinds.append(kind)
                weights.append(weight)

        writable = sorted(self.init - set(forbidden))
        offer("load", 3)
        offer("mov", 2, bool(self.init))
        offer("binop", 3, bool(writable))
        offer("risky", 1.5, bool(writable))
        offer("unop", 1, bool(writable))
        offer("get", 1.5, bool(self.vars))
        offer("set", 1.5, bool(self.vars and self.init))
        if self.local_arrays():
            offer("aget", 2)
            offer("aset", 2, bool(self.init))
            offer("len", 1)
            offer("read", 0.3)
            offer("write", 0.3, bool(self.init))
        offer("param_access", 1.5, bool(self.param))
        offer("call", 2, bool(self.callees) and "r0" not in forbidden)
        offer("branch", 1.5, bool(self.init) and self.branches < 3)
        offer("loop", 1, self.loops < 1)
        offer("sload", 2)
        if sorted(self.strings - set(forbidden)):
            offer("slen", 1.5)
            offer("sget", 1.5, bool(self.init))
            offer("scat", 1.5)
            offer("seq", 1.5)
        kind = rng.choices(kinds, weights)[0]
        if kind == "load":
            self.load(forbidden=forbidden)
        elif kind == "mov":
            dst = self.dst_reg(forbidden)
            src = self.init_reg()
            self.emit(f"MOV {dst} {src}")
            self.init.add(dst)
            (self.strings.add if src in self.strings else self.strings.discard)(dst)
        elif kind == "binop":
            # The first operand is overwritten; loop counters are off-limits.
            target = rng.choice(writable)
            self.emit(f"{rng.choice(BIN_OPS)} {target} {self.init_reg()}")
            self.strings.discard(target)
        elif kind == "risky":
            op = rng.choice(RISKY_OPS)
            target = rng.choice(writable)
            self.strings.discard(target)
            if op in ("DIV", "MOD"):
                value = 0 if rng.random() < 0.2 else rng.choice((-7, -2, -1, 1, 2, 3, 7, 100))
            else:
                value = rng.choice((0, 1, 5, 31)) if rng.random() < 0.8 else rng.choice((32, -1, 100))
            self.emit(f"{op} {target} {self.load(value=value, forbidden=forbidden)}")
        elif kind == "unop":
            target = rng.choice(writable)
            self.emit(f"{rng.choice(UN_OPS)} {target} _")
            self.strings.discard(target)
        elif kind == "get":
            dst = self.dst_reg(forbidden)
            self.emit(f"GET {dst} {rng.choice(self.vars)[0]}")
            self.init.add(dst)
            self.strings.discard(dst)
        elif kind == "set":
            self.emit(f"SET {rng.choice(self.vars)[0]} {self.init_reg()}")
        elif kind == "len":
            dst = self.dst_reg(forbidden)
            self.emit(f"LEN {dst} {rng.choice(self.arrays)[0]}")
            self.init.add(dst)
            self.strings.discard(dst)
        elif kind == "read":
            dst = self.dst_reg(forbidden)
            self.emit(f"READ {rng.choice(self.local_arrays())[0]} {dst}")
            self.init.add(dst)
            self.strings.discard(dst)
        elif kind == "write":
            array, length = rng.choice(self.local_arrays())
            self.emit(f"WRITE {array} {self.load(value=rng.randint(0, length), forbidden=forbidden)}")
        elif kind == "aget":
            array, length = rng.choice(self.local_arrays())
            if rng.random() < 0.8:
                index = rng.randint(0, length - 1)
            else:
                index = rng.choice((length, -1, 1000000))
            index_reg = self.load(value=index, forbidden=forbidden)
            dst = self.dst_reg(forbidden)
            self.emit(f"AGET {dst} {array},{index_reg}")
            self.init.add(dst)
            self.strings.discard(dst)
        elif kind == "aset":
            array, length = rng.choice(self.local_arrays())
            if rng.random() < 0.8:
                index = rng.randint(0, length - 1)
            else:
                index = rng.choice((length, -1, 1000000))
            index_reg = self.load(value=index, forbidden=forbidden)
            self.emit(f"ASET {array} {index_reg},{self.init_reg()}")
        elif kind == "param_access":
            self.gen_param_access(forbidden)
        elif kind == "call":
            self.gen_call(forbidden)
            self.strings.discard("r0")
        elif kind == "branch":
            self.gen_branch(forbidden)
        elif kind == "loop":
            self.gen_loop(forbidden)
        elif kind == "sload":
            dst = self.dst_reg(forbidden)
            self.emit(f"SLOAD {dst} {self.gen_literal()}")
            self.init.add(dst)
            self.strings.add(dst)
        elif kind == "slen":
            dst = self.dst_reg(forbidden)
            self.emit(f"SLEN {dst} {rng.choice(sorted(self.strings - set(forbidden)))}")
            self.init.add(dst)
            self.strings.discard(dst)
        elif kind == "sget":
            dst = self.dst_reg(forbidden)
            handle = rng.choice(sorted(self.strings - set(forbidden)))
            # Mostly in range, sometimes past the end to hit the bounds fault.
            index = rng.randint(0, 5) if rng.random() < 0.8 else rng.choice((-1, 64, 1000000))
            index_reg = self.load(value=index, forbidden=forbidden + (dst, handle))
            self.emit(f"SGET {dst} {handle},{index_reg}")
            self.init.add(dst)
            self.strings.discard(dst)
        elif kind == "scat":
            handles = sorted(self.strings - set(forbidden))
            dst = self.dst_reg(forbidden)
            self.emit(f"SCAT {dst} {rng.choice(handles)},{rng.choice(handles)}")
            self.init.add(dst)
            self.strings.add(dst)
        elif kind == "seq":
            handles = sorted(self.strings - set(forbidden))
            dst = self.dst_reg(forbidden)
            self.emit(f"SEQ {dst} {rng.choice(handles)},{rng.choice(handles)}")
            self.init.add(dst)
            self.strings.discard(dst)

    def gen_statements(self, count, forbidden=()):
        for _ in range(count):
            self.gen_statement(forbidden)

    def generate(self):
        signature = f"i32/{self.arity}" if self.arity else self.rng.choice(("i32", "i32/0"))
        self.emit(f"GENE {self.name} {signature}")
        if self.param:
            self.emit(f"PARAM {self.param} r0")
        self.gen_decls()
        self.load()  # the register set is never empty again
        body = self.rng.randint(4, 14) if self.name == "main" else self.rng.randint(2, 8)
        self.gen_statements(body)
        self.emit(f"RET {self.init_reg()} _")
        self.emit("END _ _")
        return "\n".join(self.lines)


def gen_program(rng):
    parts, callees = [], []
    for i in range(rng.randint(0, 2)):
        gene = Gene(rng, f"f{i}", rng.randint(0, 4), callees, allow_param=True)
        parts.append(gene.generate())
        callees.append((gene.name, gene.arity, gene.has_param))
    main = Gene(rng, "main", rng.randint(0, 4), callees, allow_param=False)
    parts.append(main.generate())
    args = [rng.choice(I32_POOL) for _ in range(main.arity)]
    return "\n\n".join(parts) + "\n", args


def mangle_for_repair(text, rng):
    """Drop the final '_' of unambiguously fillable codons."""
    out = []
    for line in text.split("\n"):
        head = line.partition(" ")[0]
        if head in FILLABLE_HEADS and line.endswith(" _") and rng.random() < 0.5:
            out.append(line[:-2])
        else:
            out.append(line)
    return "\n".join(out)


def break_statically(text, rng):
    lines = text.split("\n")
    body = [i for i, line in enumerate(lines)
            if line and not line.startswith(("GENE", "END", "VAR", "ARRAY", "PARAM", "LABEL"))]
    index = rng.choice(body)
    lines[index] = "ZAP " + " ".join(lines[index].split(" ")[1:])
    return "\n".join(lines)


def run_mode(mode, path, args, repair):
    cmd = [str(EXE), mode, str(path), *(str(a) for a in args)]
    if repair:
        cmd.append("--repair")
    proc = subprocess.run(cmd, capture_output=True, encoding="utf-8", errors="replace", timeout=15)
    return proc.returncode, proc.stdout, proc.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--count", type=int, default=150)
    parser.add_argument("--seed", type=int, default=1)
    options = parser.parse_args()
    rng = random.Random(options.seed)
    stats = {"ok": 0, "error": 0}
    with tempfile.TemporaryDirectory(prefix="fuzz-", dir=WORK) as tmp:
        case = Path(tmp) / "case.dna"
        for i in range(options.count):
            text, args = gen_program(rng)
            repair = rng.random() < 0.25
            if repair:
                text = mangle_for_repair(text, rng)
            elif rng.random() < 0.08:
                text = break_statically(text, rng)
            case.write_text(text, encoding="utf-8")
            results = {mode: run_mode(mode, case, args, repair) for mode in ("run", "interpret")}
            if results["run"] != results["interpret"]:
                repro = WORK / f"fuzz_repro_{options.seed}_{i}.dna"
                repro.write_text(text, encoding="utf-8")
                print(f"MISMATCH at case {i} (seed {options.seed}), repro: {repro}")
                print(f"args: {args} repair: {repair}")
                for mode in ("run", "interpret"):
                    code, out, err = results[mode]
                    print(f"--- {mode}: exit={code}\nstdout: {out!r}\nstderr: {err!r}")
                return 1
            stats["ok" if results["run"][0] == 0 else "error"] += 1
            if (i + 1) % 50 == 0:
                print(f"fuzz {i + 1}/{options.count}: {stats['ok']} ok, {stats['error']} errors, all agree")
    print(f"fuzz_diff: {options.count} programs, {stats['ok']} ran, {stats['error']} failed; "
          "JIT and interpreter agree on every case")
    return 0


if __name__ == "__main__":
    sys.exit(main())
