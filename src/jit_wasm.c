/* Portable WebAssembly backend for Codons.
 *
 * Generates a self-contained C program from a BioProgram and compiles it to
 * WebAssembly with clang --target=wasm32.  The compiler is taken from the
 * CODONS_CLANG environment variable, falling back to "clang" on PATH.  Each
 * live gene becomes a C function using a local register file; labels and
 * branches become C labels and gotos; function calls become C function calls.
 * The resulting .wasm module exports a `run` function that takes up to four
 * i32 arguments and returns the i32 result of the main gene.
 *
 * This backend needs no native JIT and is available in every build, including
 * the portable -DCODONS_NO_JIT=ON one.
 *
 * Covered: arithmetic, logic, shifts, comparisons, branches, calls/returns.
 * Runtime faults map to wasm traps: division by zero, division overflow and
 * out-of-range shift counts abort the execution (a RuntimeError in the host)
 * instead of returning a value, matching the native "the run fails" outcome;
 * the fault *kind* text is not reproduced.
 *
 * Not covered: local variables/arrays (GET/SET/AGET/ASET/LEN), wide ops,
 * READ/WRITE, PRINT, and string opcodes.  The step budget is NOT enforced:
 * a looping program loops forever where the native backends would stop after
 * BIO_EXECUTION_LIMIT steps.  Recursion depth is bounded by the wasm stack,
 * not by BIO_MAX_DEPTH.
 */
#include "internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int wasm_op_supported(uint8_t op) {
    switch (op) {
    case OP_LOAD: case OP_MOV: case OP_ADD: case OP_SUB: case OP_MUL:
    case OP_DIV: case OP_MOD: case OP_AND: case OP_OR: case OP_XOR: case OP_NOT:
    case OP_LAND: case OP_LOR: case OP_LXOR: case OP_LNOT:
    case OP_SHL: case OP_SHR: case OP_SAR:
    case OP_EQ: case OP_LT: case OP_MIN: case OP_MAX:
    case OP_JMP: case OP_JZ: case OP_JNZ: case OP_LABEL:
    case OP_CALL: case OP_RET:
    case OP_VAR: case OP_ARRAY: case OP_PARAM:
        return 1;
    default:
        return 0;
    }
}

static uint32_t node_index(const BioGene *g, const CodonNode *target) {
    uint32_t idx = 0;
    const CodonNode *n;
    for (n = g->head->next; n; n = n->next) {
        if (n == target) return idx;
        ++idx;
    }
    return 0;
}

/* Gene names are source identifiers and may contain dots (a.b); the C name is
 * the unique gene index, with the source name kept as a comment for audits. */
static void emit_gene_ref(FILE *f, const BioProgram *p, unsigned gene) {
    fprintf(f, "gene_%u /* %s */", gene, p->genes[gene].name);
}

static void emit_bin_op(FILE *f, const char *op, uint32_t a1, uint32_t a2, int lit) {
    fprintf(f, "  r%u = (r%u %s ", a1, a1, op);
    if (lit) fprintf(f, "(uint32_t)(%ld));\n", (long)(int32_t)a2);
    else fprintf(f, "r%u);\n", a2);
}

static int generate_program_c(const BioProgram *p, const char *path, BioError *e) {
    FILE *f;
    unsigned i;
    f = bio_fopen(path, "w");
    if (!f) { bio_error(e, "cannot write %s", path); return 0; }

    fprintf(f, "/* Auto-generated Codons WASM program */\n");
    fprintf(f, "#include <stdint.h>\n");
    fprintf(f, "typedef uint32_t u32;\n");
    fprintf(f, "typedef int32_t s32;\n");
    fprintf(f, "typedef int64_t s64;\n\n");

    for (i = 0; i < p->gene_count; ++i) {
        if (!p->genes[i].live) continue;
        fprintf(f, "static s32 ");
        emit_gene_ref(f, p, i);
        fprintf(f, "(u32,u32,u32,u32);\n");
    }
    fprintf(f, "\n");

    for (i = 0; i < p->gene_count; ++i) {
        const BioGene *g = &p->genes[i];
        const CodonNode *n;
        uint32_t idx = 0;
        unsigned j;
        if (!g->live) continue;
        fprintf(f, "static s32 gene_%u(u32 _a0, u32 _a1, u32 _a2, u32 _a3) { /* %s */\n", i, g->name);
        for (j = 0; j < g->reg_count; ++j) {
            if (j < 4)
                fprintf(f, "  u32 r%u = _a%u;\n", j, j);
            else
                fprintf(f, "  u32 r%u = 0;\n", j);
        }
        for (n = g->head->next; n; n = n->next) {
            uint8_t op = n->op_code;
            uint32_t a1 = n->arg1, a2 = n->arg2;
            int lit = n->arg2_literal;
            fprintf(f, "L%u:\n", idx);
            switch (op) {
            case OP_LABEL:
            case OP_VAR:
            case OP_ARRAY:
            case OP_PARAM:
                break;
            case OP_LOAD:
                fprintf(f, "  r%u = (u32)(%ld);\n", a1, (long)(int32_t)a2);
                break;
            case OP_MOV:
                fprintf(f, "  r%u = ", a1);
                if (lit) fprintf(f, "(u32)(%ld);\n", (long)(int32_t)a2);
                else fprintf(f, "r%u;\n", a2);
                break;
            case OP_ADD: emit_bin_op(f, "+", a1, a2, lit); break;
            case OP_SUB: emit_bin_op(f, "-", a1, a2, lit); break;
            case OP_MUL: emit_bin_op(f, "*", a1, a2, lit); break;
            case OP_AND: emit_bin_op(f, "&", a1, a2, lit); break;
            case OP_OR:  emit_bin_op(f, "|", a1, a2, lit); break;
            case OP_XOR: emit_bin_op(f, "^", a1, a2, lit); break;
            case OP_DIV: case OP_MOD: {
                /* Native faults map to traps: the divisor is guarded against
                   zero, and 64-bit math keeps INT32_MIN / -1 defined so the
                   overflow check can trap exactly like BIO_FAULT_DIV_OVERFLOW. */
                fprintf(f, "  { s64 _b = ");
                if (lit) fprintf(f, "(s64)(%ld)", (long)(int32_t)a2);
                else fprintf(f, "(s64)(s32)r%u", a2);
                fprintf(f, "; if (_b == 0) __builtin_trap();\n");
                fprintf(f, "    s64 _q = (s64)(s32)r%u %s _b;\n", a1, op == OP_DIV ? "/" : "%");
                if (op == OP_DIV)
                    fprintf(f, "    if (_q > 2147483647LL || _q < -2147483648LL) __builtin_trap();\n");
                fprintf(f, "    r%u = (u32)(s32)_q; }\n", a1);
                break;
            }
            case OP_NOT:
                fprintf(f, "  r%u = ~r%u;\n", a1, a1);
                break;
            case OP_LAND:
                fprintf(f, "  r%u = ((!!r%u) & (!!", a1, a1);
                if (lit) fprintf(f, "%ld));\n", (long)(int32_t)a2);
                else fprintf(f, "r%u));\n", a2);
                break;
            case OP_LOR:
                fprintf(f, "  r%u = ((!!r%u) | (!!", a1, a1);
                if (lit) fprintf(f, "%ld));\n", (long)(int32_t)a2);
                else fprintf(f, "r%u));\n", a2);
                break;
            case OP_LXOR:
                fprintf(f, "  r%u = ((!!r%u) ^ (!!", a1, a1);
                if (lit) fprintf(f, "%ld));\n", (long)(int32_t)a2);
                else fprintf(f, "r%u));\n", a2);
                break;
            case OP_LNOT:
                fprintf(f, "  r%u = !r%u;\n", a1, a1);
                break;
            case OP_SHL: case OP_SHR: case OP_SAR: {
                /* A literal count is compile-time bounded to 0..31; a dynamic
                   count traps above 31 exactly like BIO_FAULT_SHIFT. */
                if (!lit) fprintf(f, "  if (r%u > 31u) __builtin_trap();\n", a2);
                fprintf(f, "  r%u = ", a1);
                if (op == OP_SAR) fprintf(f, "(u32)((int32_t)r%u >> ", a1);
                else fprintf(f, "(r%u %s ", a1, op == OP_SHL ? "<<" : ">>");
                if (lit) fprintf(f, "%u)", (unsigned)(a2 & 31u));
                else fprintf(f, "r%u)", a2);
                fprintf(f, ";\n");
                break;
            }
            case OP_EQ:
                fprintf(f, "  r%u = (r%u == ", a1, a1);
                if (lit) fprintf(f, "(u32)(%ld));\n", (long)(int32_t)a2);
                else fprintf(f, "r%u);\n", a2);
                break;
            case OP_LT:
                fprintf(f, "  r%u = ((int32_t)r%u < ", a1, a1);
                if (lit) fprintf(f, "(int32_t)(%ld));\n", (long)(int32_t)a2);
                else fprintf(f, "(int32_t)r%u);\n", a2);
                break;
            case OP_MIN:
                fprintf(f, "  r%u = ((int32_t)r%u < (int32_t)", a1, a1);
                if (lit) fprintf(f, "%ld) ? r%u : (u32)(%ld);\n", (long)(int32_t)a2, a1, (long)(int32_t)a2);
                else fprintf(f, "r%u) ? r%u : r%u;\n", a2, a1, a2);
                break;
            case OP_MAX:
                fprintf(f, "  r%u = ((int32_t)r%u > (int32_t)", a1, a1);
                if (lit) fprintf(f, "%ld) ? r%u : (u32)(%ld);\n", (long)(int32_t)a2, a1, (long)(int32_t)a2);
                else fprintf(f, "r%u) ? r%u : r%u;\n", a2, a1, a2);
                break;
            case OP_JMP:
                fprintf(f, "  goto L%u;\n", (unsigned)node_index(g, n->bond));
                break;
            case OP_JZ:
                fprintf(f, "  if (!r%u) goto L%u;\n", a1, (unsigned)node_index(g, n->bond));
                break;
            case OP_JNZ:
                fprintf(f, "  if (r%u) goto L%u;\n", a1, (unsigned)node_index(g, n->bond));
                break;
            case OP_RET:
                fprintf(f, "  return (s32)r%u;\n", a1);
                break;
            case OP_CALL: {
                const BioGene *callee = &p->genes[n->arg1];
                unsigned k;
                fprintf(f, "  { u32 _a[4] = {0,0,0,0}; ");
                for (k = 0; k < callee->arity; ++k) {
                    unsigned src = (n->arg2 >> (8 * k)) & 255u;
                    fprintf(f, "_a[%u]=r%u; ", k, src);
                }
                fprintf(f, "r0 = ");
                emit_gene_ref(f, p, n->arg1);
                fprintf(f, "(_a[0],_a[1],_a[2],_a[3]); }\n");
                break;
            }
            default:
                fprintf(f, "  /* unsupported op %u */\n", op);
            }
            ++idx;
        }
        fprintf(f, "  return 0;\n");
        fprintf(f, "}\n\n");
    }

    {
        fprintf(f, "s32 run(s32 _0, s32 _1, s32 _2, s32 _3) {\n");
        fprintf(f, "  (void)_1; (void)_2; (void)_3;\n");
        fprintf(f, "  return gene_%u((u32)_0, (u32)_1, (u32)_2, (u32)_3); /* %s */\n",
                p->main_gene, p->genes[p->main_gene].name);
        fprintf(f, "}\n");
    }
    fclose(f);
    return 1;
}

int bio_jit_wasm(const BioProgram *p, const char *path, BioError *e) {
    char cfile[1024], cmd[2048];
    const char *clang;
    unsigned i;
    if (!p) { bio_error(e, "program is required"); return 0; }
    for (i = 0; i < p->gene_count; ++i) {
        const BioGene *g = &p->genes[i];
        const CodonNode *n;
        if (!g->live) continue;
        for (n = g->head->next; n; n = n->next) {
            if (n->op_code == OP_LABEL || n->op_code == OP_VAR || n->op_code == OP_ARRAY || n->op_code == OP_PARAM) continue;
            if (!wasm_op_supported(n->op_code)) {
                bio_error(e, "wasm backend does not support opcode %u at %s:%u", n->op_code,
                          p->files[n->file].path, n->line);
                return 0;
            }
        }
    }
    if (snprintf(cfile, sizeof(cfile), "%s.c", path) >= (int)sizeof(cfile)) {
        bio_error(e, "path too long"); return 0;
    }
    if (!generate_program_c(p, cfile, e)) return 0;
    clang = getenv("CODONS_CLANG");
    if (!clang || !*clang) clang = "clang";
#ifdef _WIN32
    /* cmd /c strips the first and last quote on the line, mangling quoted
       paths; wrapping the whole command in an extra quote pair prevents it. */
    if (snprintf(cmd, sizeof(cmd),
                 "\"\"%s\" --target=wasm32-unknown-unknown "
                 "-nostdlib -Wl,--no-entry -Wl,--export=run -O0 -o \"%s\" \"%s\"\"",
                 clang, path, cfile) >= (int)sizeof(cmd)) {
#else
    if (snprintf(cmd, sizeof(cmd),
                 "\"%s\" --target=wasm32-unknown-unknown "
                 "-nostdlib -Wl,--no-entry -Wl,--export=run -O0 -o \"%s\" \"%s\"",
                 clang, path, cfile) >= (int)sizeof(cmd)) {
#endif
        bio_error(e, "command too long"); return 0;
    }
    if (system(cmd) != 0) {
        bio_error(e, "failed to compile wasm with clang (set CODONS_CLANG); generated C in %s", cfile);
        return 0;
    }
    return 1;
}
