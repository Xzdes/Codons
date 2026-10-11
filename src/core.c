#include "internal.h"
#include <stdarg.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void bio_error(BioError *e, const char *format, ...) {
    va_list ap;
    int length;
    if (!e) return;
    va_start(ap, format);
    length = vsnprintf(e->message, sizeof(e->message), format, ap);
    va_end(ap);
    if (length >= (int)sizeof(e->message)) {
        size_t end = strlen(e->message), start = end - 1;
        unsigned char lead;
        size_t needed;
        while (start && ((unsigned char)e->message[start] & 0xc0u) == 0x80u) --start;
        lead = (unsigned char)e->message[start];
        needed = lead < 0x80u ? 1u : lead < 0xe0u ? 2u : lead < 0xf0u ? 3u : 4u;
        if (end - start < needed) e->message[start] = '\0';
    }
}

void bio_node_error(const BioProgram *p, const CodonNode *n, BioError *e, const char *format, ...) {
    char detail[384];
    va_list ap;
    va_start(ap, format);
    vsnprintf(detail, sizeof(detail), format, ap);
    va_end(ap);
    bio_error(e, "%s:%u: %s", p->files[n->file].path, n->line, detail);
}

CodonNode *bio_node_new(BioProgram *p, BioError *e) {
    CodonNode *n;
    if (p->stats.nodes == BIO_MAX_NODES) {
        bio_error(e, "apoptosis: node budget exceeded (%u)", BIO_MAX_NODES);
        return NULL;
    }
    if (!p->arena || p->arena->used == BIO_NODES_PER_BLOCK) {
        ArenaBlock *block = calloc(1, sizeof(*block));
        if (!block) { bio_error(e, "out of memory allocating arena"); return NULL; }
        block->next = p->arena;
        p->arena = block;
    }
    n = &p->arena->nodes[p->arena->used++];
    n->index = (uint32_t)p->stats.nodes++;
    return n;
}

void bio_program_retain(BioProgram *p) { InterlockedIncrement(&p->references); }

void bio_program_release(BioProgram *p) {
    if (!p || InterlockedDecrement(&p->references) != 0) return;
    if (p->unwind_registered) RtlDeleteFunctionTable(p->unwind_table);
    if (p->code) VirtualFree(p->code, 0, MEM_RELEASE);
    free(p->unwind_table);
    while (p->arena) {
        ArenaBlock *next = p->arena->next;
        free(p->arena);
        p->arena = next;
    }
    free(p);
}

int bio_wide_path(const char *path, wchar_t *out, size_t capacity) {
    return path && capacity <= INT_MAX &&
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, out, (int)capacity) > 0;
}

FILE *bio_fopen(const char *path, const wchar_t *mode) {
    wchar_t wide[BIO_PATH_SIZE];
    if (!bio_wide_path(path, wide, BIO_PATH_SIZE)) return NULL;
    return _wfopen(wide, mode);
}

BioProgram *bio_load(const char *path, int repair, BioError *e) {
    BioProgram *p = calloc(1, sizeof(*p));
    if (e) e->message[0] = '\0';
    if (!p) { bio_error(e, "out of memory allocating program"); return NULL; }
    p->references = 1;
    if (!path || !bio_parse(p, path, repair, e) || !bio_analyze(p, e)) {
        if (!path) bio_error(e, "source path is required");
        bio_program_release(p);
        return NULL;
    }
    return p;
}

BioProgram *bio_compile(const char *path, int repair, BioError *e) {
    BioProgram *p = bio_load(path, repair, e);
    if (p && !bio_jit(p, e)) { bio_program_release(p); return NULL; }
    return p;
}

BioStats bio_program_stats(const BioProgram *p) { return p->stats; }

int bio_find_gene(const BioProgram *p, const char *name) {
    unsigned i;
    for (i = 0; i < p->gene_count; ++i)
        if (strcmp(p->genes[i].name, name) == 0) return (int)i;
    return -1;
}

static int validate_arguments(const BioProgram *p, const int32_t *args, size_t argc,
                              int32_t *result, BioError *e) {
    if (e) e->message[0] = '\0';
    if (!p || !result || (argc && !args)) {
        bio_error(e, "program, result and supplied arguments are required"); return 0;
    }
    if (argc != p->stats.arity) {
        bio_error(e, "main expects %u arguments, got %zu", p->stats.arity, argc); return 0;
    }
    return 1;
}

static int execution_ok(const BioProgram *p, const BioExecution *execution, BioError *e) {
    const ArenaBlock *block;
    if (execution->failed_node == UINT32_MAX) return 1;
    for (block = p->arena; block; block = block->next) {
        uint32_t first = block->nodes[0].index;
        if (execution->failed_node >= first && execution->failed_node - first < block->used) {
            bio_node_error(p, &block->nodes[execution->failed_node - first], e,
                           "execution budget exhausted after %u steps", BIO_EXECUTION_LIMIT);
            return 0;
        }
    }
    bio_error(e, "execution budget exhausted after %u steps", BIO_EXECUTION_LIMIT);
    return 0;
}

int bio_program_run(const BioProgram *p, const int32_t *args, size_t argc,
                    int32_t *result, BioError *e) {
    typedef int32_t (*Entry)(int32_t, int32_t, BioExecution *);
    BioExecution execution = {BIO_EXECUTION_LIMIT, UINT32_MAX};
    int32_t value;
    Entry fn;
    unsigned char *address;
    if (!validate_arguments(p, args, argc, result, e)) return 0;
    if (!p->code) { bio_error(e, "program has no native code; use bio_compile"); return 0; }
    address = p->code + p->genes[p->main_gene].code_offset;
    _Static_assert(sizeof(fn) == sizeof(address), "Windows x64 function pointer size");
    memcpy(&fn, &address, sizeof(fn));
    value = fn(argc > 0 ? args[0] : 0, argc > 1 ? args[1] : 0, &execution);
    if (!execution_ok(p, &execution, e)) return 0;
    *result = value;
    return 1;
}

static int32_t signed_value(uint32_t bits) {
    int32_t value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static uint32_t interpret_gene(const BioProgram *p, unsigned gene, uint32_t a, uint32_t b,
                               BioExecution *execution) {
    uint32_t r[8] = {0};
    const CodonNode *n;
    r[0] = a; r[1] = b;
    n = p->genes[gene].head->next;
    while (n) {
        const CodonNode *next = n->next;
        if (n->op_code != OP_LABEL) {
            if (!execution->remaining) { execution->failed_node = n->index; return 0; }
            --execution->remaining;
        }
        switch (n->op_code) {
        case OP_LOAD: r[n->arg1] = n->arg2; break;
        case OP_MOV: r[n->arg1] = r[n->arg2]; break;
        case OP_ADD: r[n->arg1] += r[n->arg2]; break;
        case OP_SUB: r[n->arg1] -= r[n->arg2]; break;
        case OP_MUL: r[n->arg1] *= r[n->arg2]; break;
        case OP_EQ: r[n->arg1] = r[n->arg1] == r[n->arg2]; break;
        case OP_LT: r[n->arg1] = signed_value(r[n->arg1]) < signed_value(r[n->arg2]); break;
        case OP_MIN:
            if (signed_value(r[n->arg1]) > signed_value(r[n->arg2])) r[n->arg1] = r[n->arg2];
            break;
        case OP_MAX:
            if (signed_value(r[n->arg1]) < signed_value(r[n->arg2])) r[n->arg1] = r[n->arg2];
            break;
        case OP_CALL: {
            unsigned argc = n->arg2 >> 16;
            uint32_t x = argc > 0 ? r[n->arg2 & 255u] : 0;
            uint32_t y = argc > 1 ? r[(n->arg2 >> 8) & 255u] : 0;
            r[0] = interpret_gene(p, n->arg1, x, y, execution);
            if (execution->failed_node != UINT32_MAX) return 0;
            break;
        }
        case OP_RET: return r[n->arg1];
        case OP_JMP: next = n->bond; break;
        case OP_JZ: if (!r[n->arg1]) next = n->bond; break;
        case OP_JNZ: if (r[n->arg1]) next = n->bond; break;
        default: break;
        }
        n = next;
    }
    return 0; /* Validation rejects reachable fallthrough. */
}

int bio_program_interpret(const BioProgram *p, const int32_t *args, size_t argc,
                          int32_t *result, BioError *e) {
    uint32_t bits;
    BioExecution execution = {BIO_EXECUTION_LIMIT, UINT32_MAX};
    if (!validate_arguments(p, args, argc, result, e)) return 0;
    bits = interpret_gene(p, p->main_gene, argc > 0 ? (uint32_t)args[0] : 0,
                          argc > 1 ? (uint32_t)args[1] : 0, &execution);
    if (!execution_ok(p, &execution, e)) return 0;
    *result = signed_value(bits);
    return 1;
}

void bio_program_dump(const BioProgram *p, FILE *out) {
    static const char *names[] = {
        "GENE", "LOAD", "MOV", "ADD", "SUB", "MUL", "CALL", "RET", "EQ", "LT", "MIN", "MAX",
        "LABEL", "JMP", "JZ", "JNZ"
    };
    unsigned i;
    for (i = 0; i < p->gene_count; ++i) {
        const BioGene *g = &p->genes[i];
        const CodonNode *n;
        fprintf(out, "GENE %s i32/%u # %s\n", g->name, g->arity, g->live ? "live" : "pruned");
        for (n = g->head->next; n; n = n->next) {
            if (n->op_code == OP_CALL) {
                unsigned argc = n->arg2 >> 16;
                fprintf(out, "CALL %s ", p->genes[n->arg1].name);
                if (!argc) fputc('_', out);
                else {
                    fprintf(out, "r%u", n->arg2 & 255u);
                    if (argc == 2) fprintf(out, ",r%u", (n->arg2 >> 8) & 255u);
                }
            } else if (n->op_code == OP_LOAD)
                fprintf(out, "LOAD r%u %ld", n->arg1, (long)signed_value(n->arg2));
            else if (n->op_code == OP_RET) fprintf(out, "RET r%u _", n->arg1);
            else if (n->op_code == OP_LABEL || n->op_code == OP_JMP)
                fprintf(out, "%s %s _", names[n->op_code], p->symbols[n->arg1]);
            else if (n->op_code == OP_JZ || n->op_code == OP_JNZ)
                fprintf(out, "%s r%u %s", names[n->op_code], n->arg1, p->symbols[n->arg2]);
            else fprintf(out, "%s r%u r%u", names[n->op_code], n->arg1, n->arg2);
            fprintf(out, " # line %u\n", n->line);
        }
        fputs("END _ _\n\n", out);
    }
}

int bio_program_write_code(const BioProgram *p, const char *path, BioError *e) {
    FILE *f;
    int ok;
    if (!p || !p->code) { bio_error(e, "program has no native code"); return 0; }
    f = bio_fopen(path, L"wb");
    if (!f) { bio_error(e, "cannot write %s", path); return 0; }
    ok = fwrite(p->code, 1, p->stats.code_bytes, f) == p->stats.code_bytes;
    if (fclose(f) != 0) ok = 0;
    if (!ok) bio_error(e, "failed writing %s", path);
    return ok;
}
