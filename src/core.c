#include "internal.h"
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

void bio_error(BioError *e, const char *format, ...) {
    va_list ap;
    if (!e) return;
    va_start(ap, format);
    vsnprintf(e->message, sizeof(e->message), format, ap);
    va_end(ap);
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
    p->stats.nodes++;
    return &p->arena->nodes[p->arena->used++];
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

BioProgram *bio_compile(const char *path, int repair, BioError *e) {
    BioProgram *p = calloc(1, sizeof(*p));
    if (e) e->message[0] = '\0';
    if (!p) { bio_error(e, "out of memory allocating program"); return NULL; }
    p->references = 1;
    if (!path || !bio_parse(p, path, repair, e) || !bio_analyze(p, e) || !bio_jit(p, e)) {
        if (!path) bio_error(e, "source path is required");
        bio_program_release(p);
        return NULL;
    }
    return p;
}

BioStats bio_program_stats(const BioProgram *p) { return p->stats; }

int bio_find_gene(const BioProgram *p, const char *name) {
    unsigned i;
    for (i = 0; i < p->gene_count; ++i)
        if (strcmp(p->genes[i].name, name) == 0) return (int)i;
    return -1;
}

int32_t bio_program_run(const BioProgram *p) {
    typedef int32_t (*Entry)(void);
    Entry fn;
    unsigned char *address = p->code + p->genes[p->main_gene].code_offset;
    _Static_assert(sizeof(fn) == sizeof(address), "Windows x64 function pointer size");
    memcpy(&fn, &address, sizeof(fn));
    return fn();
}

static uint32_t interpret_gene(const BioProgram *p, unsigned gene, uint32_t a, uint32_t b) {
    uint32_t r[8] = {0};
    const CodonNode *n;
    r[0] = a; r[1] = b;
    for (n = p->genes[gene].head->next; n; n = n->next) {
        switch (n->op_code) {
        case OP_LOAD: r[n->arg1] = n->arg2; break;
        case OP_MOV: r[n->arg1] = r[n->arg2]; break;
        case OP_ADD: r[n->arg1] += r[n->arg2]; break;
        case OP_SUB: r[n->arg1] -= r[n->arg2]; break;
        case OP_MUL: r[n->arg1] *= r[n->arg2]; break;
        case OP_CALL: {
            unsigned argc = n->arg2 >> 16;
            uint32_t x = argc > 0 ? r[n->arg2 & 255u] : 0;
            uint32_t y = argc > 1 ? r[(n->arg2 >> 8) & 255u] : 0;
            r[0] = interpret_gene(p, n->arg1, x, y);
            break;
        }
        case OP_RET: return r[n->arg1];
        default: break;
        }
    }
    return 0; /* Validation ensures every gene has RET. */
}

int32_t bio_program_interpret(const BioProgram *p) {
    uint32_t bits = interpret_gene(p, p->main_gene, 0, 0);
    int32_t result;
    memcpy(&result, &bits, sizeof(result));
    return result;
}

void bio_program_dump(const BioProgram *p, FILE *out) {
    static const char *names[] = {"GENE", "LOAD", "MOV", "ADD", "SUB", "MUL", "CALL", "RET"};
    unsigned i;
    for (i = 0; i < p->gene_count; ++i) {
        const BioGene *g = &p->genes[i];
        const CodonNode *n;
        fprintf(out, "GENE %s i32/%u [%s] offset=%u\n", g->name, g->arity,
                g->live ? "live" : "pruned", g->code_offset);
        for (n = g->head->next; n; n = n->next) {
            fprintf(out, "  %s %u %u", names[n->op_code], n->arg1, n->arg2);
            if (n->bond) fprintf(out, " -> %s", p->genes[n->arg1].name);
            fprintf(out, " (line %u)\n", n->line);
        }
    }
}

int bio_program_write_code(const BioProgram *p, const char *path, BioError *e) {
    FILE *f = fopen(path, "wb");
    int ok;
    if (!f) { bio_error(e, "cannot write %s", path); return 0; }
    ok = fwrite(p->code, 1, p->stats.code_bytes, f) == p->stats.code_bytes;
    if (fclose(f) != 0) ok = 0;
    if (!ok) bio_error(e, "failed writing %s", path);
    return ok;
}
