# WebAssembly backend

The WASM backend is a portable second backend for Codons.  It compiles a
`.dna` program into a self-contained WebAssembly module that can run in
Node.js, a browser, or any other WASM runtime, without requiring the
Windows x64 JIT.

## Building the WASM module

```
codons emit-wasm program.dna out.wasm
```

This generates a temporary C program (`out.wasm.c`) from the compiled
`BioProgram`, then compiles it with `clang --target=wasm32-unknown-unknown
-nostdlib` to produce a `.wasm` file that exports a `run` function.

The clang binary is taken from the `CODONS_CLANG` environment variable,
falling back to `clang` on `PATH`.  The backend needs no native JIT and is
available in every build, including the portable `-DCODONS_NO_JIT=ON` one —
on Linux/macOS this is the only code-producing backend.

## Running the module

A small Node harness is provided in `tests/wasm/run.js`:

```
node tests/wasm/run.js out.wasm [arg0] [arg1] ...
```

The exported function has the signature `run(i32, i32, i32, i32) -> i32`.

## Coverage

The backend currently covers the pure-compute subset of Codons:

- i32 arithmetic: `ADD`, `SUB`, `MUL`, `DIV`, `MOD`
- Bitwise/logic: `AND`, `OR`, `XOR`, `NOT`, `LAND`, `LOR`, `LXOR`, `LNOT`
- Shifts: `SHL`, `SHR`, `SAR`
- Comparisons: `EQ`, `LT`, `MIN`, `MAX`
- Control flow: `JMP`, `JZ`, `JNZ`, `LABEL`
- Function calls/returns: `CALL`, `RET`

Not yet covered: local variables/arrays (`GET`, `SET`, `AGET`, `ASET`, `LEN`),
wide operations, `READ`/`WRITE`, `PRINT`, and string opcodes.  These require
a string/I/O runtime or memory model that is not implemented yet.

## Semantics vs the native backends

- **Runtime faults trap.**  Division by zero, division overflow
  (`INT32_MIN / -1`) and a dynamic shift count above 31 abort the wasm
  execution (`__builtin_trap` → a `RuntimeError` in Node) instead of
  returning a value.  This matches the native "the run fails" outcome,
  but the fault *kind* text is not reproduced.
- **No step budget.**  A looping program loops forever; the native backends
  would stop after `BIO_EXECUTION_LIMIT` (1,000,000) steps.
- **Recursion** is bounded by the wasm stack, not by `BIO_MAX_DEPTH` (256).
- All other covered opcodes (including literal second operands, 0.17) are
  bit-identical to the interpreter; `tests/test_wasm.py` checks this,
  including the trap equivalence above.

## Verification

The test `tests/test_wasm.py` emits a `.wasm` for selected reference programs
and compares the result against the native interpreter.  It is registered as a
ctest when Python3 is available and skips cleanly when Node (`$NODE`/PATH) or
clang (`$CODONS_CLANG`/PATH) is missing.  Generated artifacts go to the
`work/` directory, not the source tree.

## Design notes

The backend deliberately mirrors the interpreter's register and control-flow
model: each Codons gene becomes a C function, each register becomes a C local,
and branches become C labels/gotos.  This makes the generated code easy to
audit and keeps the WASM module tiny.
