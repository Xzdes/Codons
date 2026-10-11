#include "internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
    int failed;
} Emitter;

static void byte(Emitter *b, unsigned value) {
    if (b->length == b->capacity) { b->failed = 1; return; }
    b->data[b->length++] = (unsigned char)value;
}

static void word32(Emitter *b, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4; ++i) byte(b, (value >> (8 * i)) & 255u);
}

/* Each virtual register occupies a 4-byte stack slot above 32-byte shadow space. */
static unsigned slot(unsigned r) { return 32u + r * 4u; }

static void load_eax(Emitter *b, unsigned r) {
    byte(b, 0x8b); byte(b, 0x44); byte(b, 0x24); byte(b, slot(r));
}

static void store_eax(Emitter *b, unsigned r) {
    byte(b, 0x89); byte(b, 0x44); byte(b, 0x24); byte(b, slot(r));
}

static void emit_gene(Emitter *b, BioGene *g) {
    CodonNode *n;
    g->code_offset = (uint32_t)b->length;
    /* sub rsp,72: shadow space + 8 i32 slots + 8 bytes for ABI alignment. */
    byte(b, 0x48); byte(b, 0x83); byte(b, 0xec); byte(b, 0x48);
    if (g->arity > 0) {
        byte(b, 0x89); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(0));
    }
    if (g->arity > 1) {
        byte(b, 0x89); byte(b, 0x54); byte(b, 0x24); byte(b, slot(1));
    }
    for (n = g->head->next; n; n = n->next) {
        switch (n->op_code) {
        case OP_LOAD:
            byte(b, 0xc7); byte(b, 0x44); byte(b, 0x24); byte(b, slot(n->arg1));
            word32(b, n->arg2);
            break;
        case OP_MOV:
            load_eax(b, n->arg2); store_eax(b, n->arg1);
            break;
        case OP_ADD: case OP_SUB: case OP_MUL:
            load_eax(b, n->arg1);
            if (n->op_code == OP_MUL) { byte(b, 0x0f); byte(b, 0xaf); }
            else byte(b, n->op_code == OP_ADD ? 0x03 : 0x2b);
            byte(b, 0x44); byte(b, 0x24); byte(b, slot(n->arg2));
            store_eax(b, n->arg1);
            break;
        case OP_CALL: {
            unsigned argc = n->arg2 >> 16;
            if (argc > 0) {
                byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(n->arg2 & 255u));
            } else { byte(b, 0x31); byte(b, 0xc9); }
            if (argc > 1) {
                byte(b, 0x8b); byte(b, 0x54); byte(b, 0x24); byte(b, slot((n->arg2 >> 8) & 255u));
            } else { byte(b, 0x31); byte(b, 0xd2); }
            byte(b, 0xe8);
            n->patch_offset = (uint32_t)b->length;
            word32(b, 0);
            store_eax(b, 0);
            break;
        }
        case OP_RET:
            load_eax(b, n->arg1);
            byte(b, 0x48); byte(b, 0x83); byte(b, 0xc4); byte(b, 0x48); byte(b, 0xc3);
            break;
        default: break;
        }
    }
    g->code_end = (uint32_t)b->length;
}

int bio_jit(BioProgram *p, BioError *e) {
    unsigned i, live = 0;
    uint32_t unwind_offset;
    DWORD old_protection;
    Emitter b = {0};
    /* Bounded by the parser: every instruction needs fewer than 32 bytes. */
    p->allocation_size = (p->stats.nodes + p->gene_count) * 32u + 16u;
    p->code = VirtualAlloc(NULL, p->allocation_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p->code) { bio_error(e, "VirtualAlloc failed (%lu)", GetLastError()); return 0; }
    b.data = p->code; b.capacity = p->allocation_size;
    for (i = 0; i < p->gene_count; ++i)
        if (p->genes[i].live) emit_gene(&b, &p->genes[i]);
    if (b.failed) { bio_error(e, "internal error: JIT output exceeds reserved buffer"); return 0; }
    p->stats.code_bytes = b.length;
    for (i = 0; i < p->gene_count; ++i) {
        CodonNode *n;
        if (!p->genes[i].live) continue;
        for (n = p->genes[i].head->next; n; n = n->next) {
            uint32_t delta;
            unsigned j;
            if (n->op_code != OP_CALL) continue;
            delta = p->genes[n->arg1].code_offset - (n->patch_offset + 4u);
            for (j = 0; j < 4; ++j)
                p->code[n->patch_offset + j] = (unsigned char)(delta >> (8u * j));
        }
    }
    while (b.length % 4) byte(&b, 0);
    unwind_offset = (uint32_t)b.length;
    /* UNWIND_INFO v1; prolog 4 bytes; UWOP_ALLOC_SMALL for sub rsp,72. */
    byte(&b, 1); byte(&b, 4); byte(&b, 1); byte(&b, 0);
    byte(&b, 4); byte(&b, 0x82); byte(&b, 0); byte(&b, 0);
    if (b.failed) { bio_error(e, "internal error: unwind data exceeds buffer"); return 0; }
    p->unwind_table = calloc(p->stats.live_genes, sizeof(*p->unwind_table));
    if (!p->unwind_table) { bio_error(e, "out of memory allocating unwind table"); return 0; }
    for (i = 0; i < p->gene_count; ++i) {
        if (!p->genes[i].live) continue;
        p->unwind_table[live].BeginAddress = p->genes[i].code_offset;
        p->unwind_table[live].EndAddress = p->genes[i].code_end;
        p->unwind_table[live++].UnwindData = unwind_offset;
    }
    /* Never keep memory simultaneously writable and executable. */
    if (!VirtualProtect(p->code, p->allocation_size, PAGE_EXECUTE_READ, &old_protection)) {
        bio_error(e, "VirtualProtect failed (%lu)", GetLastError()); return 0;
    }
    if (!FlushInstructionCache(GetCurrentProcess(), p->code, b.length)) {
        bio_error(e, "FlushInstructionCache failed (%lu)", GetLastError()); return 0;
    }
    if (!RtlAddFunctionTable(p->unwind_table, live, (DWORD64)(uintptr_t)p->code)) {
        bio_error(e, "RtlAddFunctionTable failed"); return 0;
    }
    p->unwind_registered = 1;
    return 1;
}
