#!/usr/bin/env python3
"""Independent Python reference oracle for the Codons semantics.

Loads a .dna program by invoking `codons dump`, parses the normalized IR, and
executes it with a Python interpreter that mirrors the C implementation in
src/core.c.  Provides a readable, independent specification of the language.

Usage: python oracle.py <program.dna> [arg0 arg1 ...]
"""
from __future__ import annotations

import argparse
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional


class BioExecutionError(Exception):
    pass


@dataclass
class Local:
    name: str = ""
    offset: int = 0
    count: int = 0
    initial: int = 0
    is_array: bool = False


@dataclass
class Node:
    op: str
    arg1: int = 0
    arg2: Any = None
    arg2_literal: bool = False
    bond: Optional[int] = None
    line: int = 0


@dataclass
class Gene:
    name: str
    arity: int
    live: bool
    locals: List[Local] = field(default_factory=list)
    nodes: List[Node] = field(default_factory=list)
    reg_base: int = 0
    reg_count: int = 0
    local_cells: int = 0


@dataclass
class Program:
    genes: Dict[str, Gene]
    gene_list: List[Gene]
    main: Gene


def signed_value(v: int) -> int:
    v &= 0xFFFFFFFF
    return int(v - 0x100000000) if v >= 0x80000000 else int(v)


def parse_literal(tok: str) -> int:
    tok = tok.strip()
    v = int(tok, 16) if tok.startswith("0x") else int(tok)
    return v & 0xFFFFFFFF


def parse_ir(text: str) -> Program:
    genes: Dict[str, Gene] = {}
    gene_list: List[Gene] = []
    current: Optional[Gene] = None
    for raw in text.splitlines():
        line = raw.strip()
        if not line:
            continue
        if line.startswith("GENE "):
            parts = line.split()
            name = parts[1]
            arity = int(parts[2].split("/")[1])
            live = "live" in line
            current = Gene(name=name, arity=arity, live=live)
            genes[name] = current
            gene_list.append(current)
            continue
        if line.startswith("END") or current is None:
            continue
        comment_idx = line.find(" #")
        rest = line[:comment_idx] if comment_idx >= 0 else line
        tokens = rest.split()
        op = tokens[0]
        if op in ("VAR", "ARRAY"):
            lname = tokens[1]
            size = parse_literal(tokens[2])
            current.locals.append(Local(name=lname, count=size, is_array=(op == "ARRAY")))
            current.nodes.append(Node(op=op, arg1=len(current.locals) - 1, arg2=size))
            continue
        if op in ("LABEL", "PARAM"):
            current.nodes.append(Node(op=op, arg1=tokens[1]))
            continue
        if op == "JMP":
            current.nodes.append(Node(op=op, arg1=tokens[1]))
            continue
        if op in ("JZ", "JNZ"):
            reg = int(tokens[1][1:])
            current.nodes.append(Node(op=op, arg1=reg, arg2=tokens[2]))
            continue
        if op == "CALL":
            name = tokens[1]
            reg_tokens = " ".join(tokens[2:]).replace(",", " ").split()
            regs = [int(t[1:]) for t in reg_tokens if t.startswith("r")]
            current.nodes.append(Node(op=op, arg1=name, arg2=regs))
            continue
        if op == "RET":
            current.nodes.append(Node(op=op, arg1=int(tokens[1][1:])))
            continue
        if op in ("GET", "LEN"):
            dst = int(tokens[1][1:])
            current.nodes.append(Node(op=op, arg1=dst, arg2=tokens[2]))
            continue
        if op == "SET":
            current.nodes.append(Node(op=op, arg1=tokens[1], arg2=int(tokens[2][1:])))
            continue
        if op in ("READ", "WRITE"):
            current.nodes.append(Node(op=op, arg1=tokens[1], arg2=int(tokens[2][1:])))
            continue
        if op in ("AGET", "ASET"):
            if op == "AGET":
                dst = int(tokens[1][1:])
                parts = tokens[2].split(",")
                lname = parts[0]
                idx = int(parts[1][1:])
                current.nodes.append(Node(op=op, arg1=dst, arg2=(lname, idx)))
            else:
                lname = tokens[1]
                parts = tokens[2].split(",")
                idx = int(parts[0][1:])
                src = int(parts[1][1:])
                current.nodes.append(Node(op=op, arg1=(lname, idx), arg2=src))
            continue
        if op in ("SGET", "SCAT", "SEQ"):
            dst = int(tokens[1][1:])
            a = int(tokens[2].split(",")[0][1:])
            b = int(tokens[2].split(",")[1][1:])
            current.nodes.append(Node(op=op, arg1=dst, arg2=(a, b)))
            continue
        if op == "SLOAD":
            dst = int(tokens[1][1:])
            current.nodes.append(Node(op=op, arg1=dst, arg2=""))
            continue
        if op[0] == "W" and op not in ("WIDEN", "NARROW"):
            dst = int(tokens[1][1:])
            if op == "WNOT":
                current.nodes.append(Node(op=op, arg1=dst))
            else:
                src = int(tokens[2][1:])
                current.nodes.append(Node(op=op, arg1=dst, arg2=src))
            continue
        if op in ("WIDEN", "NARROW"):
            dst = int(tokens[1][1:])
            src = int(tokens[2][1:])
            current.nodes.append(Node(op=op, arg1=dst, arg2=src))
            continue
        if op in ("NOT", "LNOT", "PRINT"):
            current.nodes.append(Node(op=op, arg1=int(tokens[1][1:])))
            continue
        if op == "LOAD":
            dst = int(tokens[1][1:])
            val = parse_literal(tokens[2])
            current.nodes.append(Node(op=op, arg1=dst, arg2=val, arg2_literal=True))
            continue
        # binary ops
        dst = int(tokens[1][1:])
        src_tok = tokens[2]
        if src_tok.startswith("r"):
            current.nodes.append(Node(op=op, arg1=dst, arg2=int(src_tok[1:])))
        else:
            current.nodes.append(Node(op=op, arg1=dst, arg2=parse_literal(src_tok), arg2_literal=True))

    for g in gene_list:
        offset = 0
        for l in g.locals:
            l.offset = offset
            offset += l.count
        g.local_cells = offset
        g.reg_base = g.local_cells

    for g in gene_list:
        for n in g.nodes:
            if n.op in ("JMP", "JZ", "JNZ"):
                label = n.arg2 if n.op != "JMP" else n.arg1
                for idx, nn in enumerate(g.nodes):
                    if nn.op == "LABEL" and nn.arg1 == label:
                        n.bond = idx
                        break
        for n in g.nodes:
            if n.op in ("LOAD", "MOV", "ADD", "SUB", "MUL", "DIV", "MOD", "AND", "OR", "XOR", "NOT",
                        "LAND", "LOR", "LXOR", "LNOT", "SHL", "SHR", "SAR", "EQ", "LT", "MIN", "MAX",
                        "GET", "LEN", "AGET", "SGET", "WLOAD", "WMOV", "WADD", "WSUB", "WMUL",
                        "WDIV", "WMOD", "WAND", "WOR", "WXOR", "WNOT", "WSHL", "WSHR", "WSAR",
                        "WEQ", "WLT", "WMIN", "WMAX", "WIDEN", "NARROW"):
                g.reg_count = max(g.reg_count, n.arg1 + 1)
            if n.op in ("MOV", "ADD", "SUB", "MUL", "DIV", "MOD", "AND", "OR", "XOR", "LAND",
                        "LOR", "LXOR", "SHL", "SHR", "SAR", "EQ", "LT", "MIN", "MAX") and not getattr(n, "arg2_literal", False):
                g.reg_count = max(g.reg_count, n.arg2 + 1)
        g.reg_count = max(g.reg_count, g.arity)

    return Program(genes=genes, gene_list=gene_list, main=gene_list[0] if gene_list else Gene("main", 0, True))


def execute(program: Program, args: List[int]) -> int:
    BIO_EXEC_LIMIT = 1_000_000

    def run_gene(g: Gene, gene_args: List[int]) -> int:
        frame = [0] * g.local_cells
        r = [0] * g.reg_count
        w = [0] * 4
        for i, a in enumerate(gene_args[:g.arity]):
            r[i] = a & 0xFFFFFFFF
        for l in g.locals:
            if not l.is_array:
                frame[l.offset] = l.initial & 0xFFFFFFFF
        local_map = {l.name: l for l in g.locals}
        budget = BIO_EXEC_LIMIT
        i = 0
        while i < len(g.nodes):
            n = g.nodes[i]
            if n.op not in ("LABEL", "VAR", "ARRAY", "PARAM"):
                budget -= 1
                if budget < 0:
                    raise BioExecutionError("execution budget exhausted")
            op = n.op
            if op == "LOAD":
                r[n.arg1] = n.arg2
            elif op == "MOV":
                r[n.arg1] = n.arg2 if n.arg2_literal else r[n.arg2]
            elif op == "ADD":
                r[n.arg1] = (r[n.arg1] + (n.arg2 if n.arg2_literal else r[n.arg2])) & 0xFFFFFFFF
            elif op == "SUB":
                r[n.arg1] = (r[n.arg1] - (n.arg2 if n.arg2_literal else r[n.arg2])) & 0xFFFFFFFF
            elif op == "MUL":
                r[n.arg1] = (r[n.arg1] * (n.arg2 if n.arg2_literal else r[n.arg2])) & 0xFFFFFFFF
            elif op in ("DIV", "MOD"):
                a = signed_value(r[n.arg1])
                b = signed_value(n.arg2 if n.arg2_literal else r[n.arg2])
                if b == 0:
                    raise BioExecutionError("division by zero")
                v = a // b if op == "DIV" else a % b
                r[n.arg1] = v & 0xFFFFFFFF
            elif op == "AND":
                r[n.arg1] &= (n.arg2 if n.arg2_literal else r[n.arg2])
            elif op == "OR":
                r[n.arg1] |= (n.arg2 if n.arg2_literal else r[n.arg2])
            elif op == "XOR":
                r[n.arg1] ^= (n.arg2 if n.arg2_literal else r[n.arg2])
            elif op == "NOT":
                r[n.arg1] = (~r[n.arg1]) & 0xFFFFFFFF
            elif op == "LAND":
                r[n.arg1] = int(bool(r[n.arg1]) and bool(n.arg2 if n.arg2_literal else r[n.arg2]))
            elif op == "LOR":
                r[n.arg1] = int(bool(r[n.arg1]) or bool(n.arg2 if n.arg2_literal else r[n.arg2]))
            elif op == "LXOR":
                r[n.arg1] = int(bool(r[n.arg1]) != bool(n.arg2 if n.arg2_literal else r[n.arg2]))
            elif op == "LNOT":
                r[n.arg1] = int(not r[n.arg1])
            elif op in ("SHL", "SHR"):
                c = (n.arg2 if n.arg2_literal else r[n.arg2]) & 0xFFFFFFFF
                if c > 31:
                    raise BioExecutionError("shift count must be in 0..31")
                if op == "SHL":
                    r[n.arg1] = (r[n.arg1] << c) & 0xFFFFFFFF
                else:
                    r[n.arg1] = (r[n.arg1] >> c) & 0xFFFFFFFF
            elif op == "SAR":
                c = (n.arg2 if n.arg2_literal else r[n.arg2]) & 0xFFFFFFFF
                if c > 31:
                    raise BioExecutionError("shift count must be in 0..31")
                r[n.arg1] = (signed_value(r[n.arg1]) >> c) & 0xFFFFFFFF
            elif op == "EQ":
                r[n.arg1] = int(r[n.arg1] == (n.arg2 if n.arg2_literal else r[n.arg2]))
            elif op == "LT":
                a = signed_value(r[n.arg1])
                b = signed_value(n.arg2 if n.arg2_literal else r[n.arg2])
                r[n.arg1] = int(a < b)
            elif op == "MIN":
                a = signed_value(r[n.arg1])
                b = signed_value(n.arg2 if n.arg2_literal else r[n.arg2])
                r[n.arg1] = (n.arg2 if n.arg2_literal else r[n.arg2]) if a > b else r[n.arg1]
            elif op == "MAX":
                a = signed_value(r[n.arg1])
                b = signed_value(n.arg2 if n.arg2_literal else r[n.arg2])
                r[n.arg1] = (n.arg2 if n.arg2_literal else r[n.arg2]) if a < b else r[n.arg1]
            elif op == "GET":
                l = local_map[n.arg2]
                r[n.arg1] = frame[l.offset]
            elif op == "SET":
                l = local_map[n.arg1]
                frame[l.offset] = r[n.arg2]
            elif op == "LEN":
                l = local_map[n.arg2]
                r[n.arg1] = l.count
            elif op == "AGET":
                lname, idx = n.arg2
                l = local_map[lname]
                r[n.arg1] = frame[l.offset + r[idx]]
            elif op == "ASET":
                (lname, idx), src = n.arg1, n.arg2
                l = local_map[lname]
                frame[l.offset + r[idx]] = r[src]
            elif op == "JMP":
                i = n.bond
                continue
            elif op == "JZ":
                if not r[n.arg1]:
                    i = n.bond
                    continue
            elif op == "JNZ":
                if r[n.arg1]:
                    i = n.bond
                    continue
            elif op == "CALL":
                callee = program.genes[n.arg1]
                args = [r[reg] for reg in n.arg2]
                r[0] = run_gene(callee, args)
            elif op == "RET":
                return r[n.arg1]
            elif op in ("LABEL", "VAR", "ARRAY", "PARAM"):
                pass
            else:
                raise NotImplementedError(f"opcode {op}")
            i += 1
        return r[0] if r else 0

    return signed_value(run_gene(program.main, args))


def run_file(codons: Path, dna: Path, args: List[int]) -> int:
    proc = subprocess.run([str(codons), "dump", str(dna)], capture_output=True, text=True, encoding="utf-8")
    if proc.returncode != 0:
        raise RuntimeError(f"codons dump failed: {proc.stderr}")
    program = parse_ir(proc.stdout)
    return execute(program, args)


def main() -> int:
    parser = argparse.ArgumentParser(description="Codons Python reference oracle")
    parser.add_argument("program", type=Path, help=".dna source file")
    parser.add_argument("args", nargs="*", help="integer arguments")
    parser.add_argument("--codons", default="codons", help="path to codons executable")
    args = parser.parse_args()
    int_args = [int(a) for a in args.args]
    result = run_file(Path(args.codons), args.program, int_args)
    print(result)
    return 0


if __name__ == "__main__":
    sys.exit(main())
