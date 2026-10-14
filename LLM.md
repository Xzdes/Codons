# Codons — machine reference for code-generating models

Codons is a tiny compiled language for Windows x64. Programs are `.dna` text
files. Every program compiles ahead-of-time to native code and also runs on a
reference interpreter; both must agree, so any program you write has one exact
meaning. This file is the complete contract. When in doubt, imitate the idioms
at the end — do not improvise syntax.

## Absolute syntax rules

1. One instruction per line, always **exactly 3 fields** separated by spaces or
   tabs. Never more, never less.
2. Unused fields are written as `_`: `RET r0 _`, `LABEL loop _`, `END _ _`.
3. Operand groups use commas **without spaces**: `AGET r0 items,r1`.
   `CALL add r1, r2` (space after comma) is the most common syntax error.
4. `#` starts a comment (outside quoted strings). Blank lines are fine.
5. Everything is case-sensitive. Identifiers: `[A-Za-z_][A-Za-z0-9_.]*`,
   max 63 chars; a dot is legal inside (used by STRUCT fields).
6. The third field of `RET NOT LNOT WNOT LABEL JMP PRINT IMPORT END` must be `_`.
7. Every function is `GENE name i32/N` ... `END _ _`. `N` is 0..4 arguments.
   The entry point is `GENE main i32/N`. Arguments arrive in `r0..r(N-1)`.
8. Files are UTF-8, max 4 MiB total, 511 bytes per line. Tabs are fine.

## Value classes (never mix implicitly)

| Class | Names | Width | Notes |
|---|---|---|---|
| Registers | `r0`..`r255` | i32 | Per call frame. Result of CALL lands in `r0`. |
| Wide registers | `w0`..`w3` | i64 | Separate class. Cross only via WIDEN/NARROW. |
| String handles | live in rN | opaque i32 | From SLOAD/SCAT. Only SLEN/SGET/SEQ/SCAT/PRINT touch them. |
| Arrays/vars | identifiers | i32 cells | Declared per gene. |

Registers are **not** zero-initialized: you must write before you read on every
execution path (the compiler proves it; see "Initialization" below).
Declared locals (`VAR`/`ARRAY`/`STRUCT` fields) are zero-initialized and cost
one step each per call.

## Grammar

```
program    := { import | gene }
import     := "IMPORT" path "_"
gene       := "GENE" ident ("i32" | "i32/" digit) { decl | codon } "END" "_" "_"
decl       := "VAR" ident int32 | "ARRAY" ident len | "PARAM" ident "r0"
            | "STRUCT" ident ident{"," ident}        # decls precede all codons
codon      := op a b                                  # always 3 fields
```

## Opcodes

`dst` is a register written; `src` a register read; `lit` a decimal i32 literal
(e.g. `5`, `-2147483648`). **Every binary i32 op accepts `src` OR `lit` as its
second operand** — `ADD r0 5` and `ADD r0 r1` are both legal.

```
LOAD dst lit            dst = lit
MOV  dst src|lit        dst = src
ADD/SUB/MUL dst src|lit dst op= src            (wraps mod 2^32)
DIV/MOD dst src|lit     signed, trunc toward 0; fault: division by zero;
                        INT32_MIN / -1 -> fault "division overflow".
                        A literal 0 divisor is a COMPILE error.
AND/OR/XOR dst src|lit  bitwise, wraps
NOT dst _               dst = ~dst
LAND/LOR/LXOR dst s|l   logical on truthiness, result 0/1
LNOT dst _              dst = !dst
SHL/SHR/SAR dst src|lit shift; count 0..31 else fault. A literal count
                        outside 0..31 is a COMPILE error. SAR is arithmetic.
EQ/LT dst src|lit       dst = 0/1; LT is signed
MIN/MAX dst src|lit     signed
LABEL name _            local jump target (per-gene namespace)
JMP name _
JZ  src name            jump if src == 0
JNZ src name            jump if src != 0
RET src _               return src (must be reachable on every path)
CALL name _             call gene with arity 0; result in r0 (clobbered!)
CALL name r1,r2         up to 4 register args, must match callee arity
CALL name arr,rN,..     first operand an array + its length in rN
VAR name lit            1 cell, initial value lit
ARRAY name len          len cells (1..65536 total per gene)
PARAM name r0           borrows caller's array; length arrives in r0
STRUCT name f1,f2       sugar: declares VAR name.f1 0, VAR name.f2 0
GET dst name            read local; AGET dst arr,idx; LEN dst arr
SET name src            write local; ASET arr idx,src
READ arr dst            fill arr from host input list; dst = count read
WRITE arr src           output src cells of arr as the result list
SLOAD dst "text"        string literal; escapes: \n \t \\ \" \xHH
SLEN dst src            dst = byte length of string in src
SGET dst s,idx          dst = byte at idx
SCAT dst a,b            dst = concatenation (new handle)
SEQ dst a,b             dst = 0/1 equality
PRINT src _             write string bytes to stdout (needs host capability)
WLOAD wN lit            i64 literal, full range
WMOV/WADD/.../WMAX wN wM   i64 arithmetic (WDIV/WMOD signed; faults mirror i32)
WSHL/WSHR/WSAR wN wM    count 0..63 else fault
WEQ/WLT dst wA,wB       compare i64, dst (i32 register) = 0/1
WIDEN wN src            i32 -> i64 sign-extend
NARROW dst wN           i64 -> i32; fault if it does not fit
```

## Initialization rule (the one models break most)

A register/wide-register may be read only if it is written on **every** path
leading to the read. Function arguments `r0..r(N-1)` start initialized.
After a branch, only registers written on **both** arms count as initialized.
Registers written only inside a loop body are **not** initialized after the
loop (the loop may run zero times).

```
LOAD r1 0          # initialize BEFORE the loop
LABEL loop _
...
JNZ r2 loop
RET r1 _           # legal: r1 was written before and on every path
```

Error text: `read of uninitialized register r1 on a reachable path`.

## Canonical idioms — copy these shapes

### if / else
```
JZ r0 else
# then-arm
JMP done _
LABEL else _
# else-arm
LABEL done _
```

### countdown loop (accumulator initialized before the loop, zero-trip guard)
```
LOAD r1 0          # acc
JZ r0 done         # n == 0 -> skip the loop entirely
LABEL loop _
ADD r1 r0          # acc += n
SUB r0 1           # literal operands are fine
JNZ r0 loop
LABEL done _
RET r1 _
```
Without the `JZ` guard a do-while shape runs once even for n = 0 and the
counter underflows to -1: an infinite loop.

### clamp x to [lo, hi]
```
MAX r0 r1
MIN r0 r2
RET r0 _
```

### array sum (local array)
```
ARRAY a 4
LOAD r1 0          # index
LOAD r2 0          # sum
LABEL loop _
MOV r4 r1
LT r4 4            # r4 = (i < 4); comparisons OVERWRITE their first operand,
JZ r4 done         # so always compare a COPY of the index, never the index
AGET r3 a,r1
ADD r2 r3
ADD r1 1
JMP loop _
LABEL done _
RET r2 _
```

### struct fields
```
STRUCT point x,y
LOAD r1 40
SET point.x r1
GET r2 point.x
ADD r2 2
RET r2 _
```
The bare name `point` is not a local; only `point.x`, `point.y` exist.

### recursion (max 256 active frames; frames are fully fresh)
```
GENE fact i32/1
JZ r0 base
MOV r1 r0
SUB r0 1
CALL fact r0       # result in r0; caller's other registers survive
MUL r1 r0
MOV r0 r1
RET r0 _
LABEL base _
LOAD r0 1
RET r0 _
END _ _
```

### print a string
```
SLOAD r1 "hello\n"
PRINT r1 _
```

### list in / list out (CLI: --input numbers.txt --output out.txt)
```
ARRAY buf 100
READ buf r1        # r1 = count actually read (<= capacity)
# ... process buf[0..r1) ...
WRITE buf r1
```

### CSV self-test (the agent loop's ground truth)
`codons test prog.dna cases.csv` where cases.csv:
```
arg1,arg2,expected
3,4,12
0,5,0
```
Header forms: `expected` | `arg1,expected` | `arg1,arg2,expected` (arity of main).
Add `input` before `expected` (a list file) and `expected_output` after it for
list programs. List paths are relative to the CSV.

## Semantic pitfalls (these compile fine but compute garbage)

- **Comparisons overwrite their first operand.** `EQ r1 r2`, `LT r1 4`,
  `MIN`, `MAX` all write `r1`. To keep a value, compare a copy:
  `MOV r3 r1` then `LT r3 4`. Killing a loop counter or an input argument
  this way is the top silent bug.
- **A bottom-of-loop `JNZ r1 loop` must see the counter itself,** not a
  comparison flag left in the counter's register.
- **Guard zero-trip loops.** A body that reads or decrements the counter
  before checking it runs once even for n = 0; `SUB r0 1` on 0 gives -1 and
  `JNZ` loops forever. Put `JZ r0 done` before (or right after) the label.
- **CALL clobbers r0.** Save it before the call if you still need it.
- **MOD sign follows the dividend:** `-7 MOD 3` is `-1`.
- **NARROW faults** when the i64 does not fit; compare with WLT first.

## Pitfalls (exact error → fix)

- `expected exactly 3 fields; found 4` → you wrote a space after a comma.
  Write `CALL add r1,r2`.
- `found 2` → missing field; pad with `_`: `RET r0 _`.
- `read of uninitialized register rN` → write it before this point on every
  path (typically `LOAD rN 0` after GENE or before the loop).
- `undefined local x in gene main` → declare `VAR x 0` / `ARRAY x 8` /
  `STRUCT` before the first instruction (decls come first, always).
- `undefined label L in gene main` → labels are per-gene; add `LABEL L _`.
- `undefined gene: f` → declare `GENE f i32/N ... END _ _` or fix spelling.
- `gene f expects 2 arguments, got 3` → match the callee's arity (max 4).
- `CALL accepts at most four register arguments` → pass an ARRAY instead.
- `literal shift count must be in 0..31` → use a register count for dynamic
  shifts, or fix the constant.
- `division by literal zero` → constants are checked at compile time.
- `NARROW` fault `wide value does not fit in i32` → guard with WLT before
  narrowing.
- Declarations after instructions → `must precede all instructions and labels`.

## Agent workflow

```
codons check prog.dna --json     # fastest validation; errors are JSON:
                                 # {"ok":false,"error":{"stage":..,"message":..,
                                 #   "line_text":..}}  line_text = your source line
codons test prog.dna cases.csv   # semantic check against examples
codons verify prog.dna 3 4       # JIT and interpreter must agree
codons run prog.dna 3 4 --json   # result + stats as JSON
codons run prog.dna --repair     # only fixes missing _ fields and quarantines
                                 # ambiguous codons; never guesses semantics
```

Limits to design around: 256 registers per gene, 64 declared locals per gene,
65 536 i32 cells per gene, 1 MiB of live frame data per call chain, recursion
depth 256, 1 000 000 execution steps per run, strings 512 KiB per run,
lists 65 536 elements, 4 call arguments.
