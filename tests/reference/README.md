# Python reference oracle

This directory contains an independent Python implementation of the Codons
semantics.  It is intended as a readable executable specification and as a
verification tool for the native interpreter/JIT.

## Files

- `oracle.py` — parses the normalized IR dump produced by `codons dump` and
  executes it with a Python interpreter that mirrors `src/core.c`.
- `driver.py` — compares the oracle against the native interpreter for a
  curated set of reference programs.
- `README.md` — this file.

## Usage

Run a single program through the oracle:

```
python tests/reference/oracle.py tests/llm_eval/reference/add-two.dna 5 7
```

Run the differential driver:

```
python tests/reference/driver.py --codons build/codons.exe
```

## Coverage

The oracle covers the full pure-compute subset: i32 arithmetic, logic, shifts,
comparisons, branches, function calls/returns, local variables, and arrays.
READ/WRITE/PRINT and string opcodes are not implemented because they require a
host I/O runtime.
