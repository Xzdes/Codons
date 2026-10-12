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

static void epilog(Emitter *b) {
    byte(b, 0x48); byte(b, 0x83); byte(b, 0xc4); byte(b, BIO_STACK_SIZE); byte(b, 0xc3);
}

static void load_locals(Emitter *b) {
    byte(b, 0x4c); byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, 72); /* mov r9,[rsp+72] */
}

static void load_context(Emitter *b) {
    /* mov r8,[rsp+64]; R8 is volatile across a nested call. */
    byte(b, 0x4c); byte(b, 0x8b); byte(b, 0x44); byte(b, 0x24); byte(b, 64);
}

/* Jump past a normal error return when the x86 condition is satisfied.
   R8 points to this invocation's context, restored by guard_step.
   Near (rel32) form: guards stay valid however large the function grows. */
static void require_condition(Emitter *b, const CodonNode *n, BioFault fault, unsigned condition) {
    size_t patch, distance, j;
    byte(b, 0x0f); byte(b, (condition & 0x0fu) | 0x80u); patch = b->length; word32(b, 0);
    byte(b, 0x41); byte(b, 0xc7); byte(b, 0x40); byte(b, 8); word32(b, n->index);
    byte(b, 0x41); byte(b, 0xc7); byte(b, 0x40); byte(b, 12); word32(b, (uint32_t)fault);
    byte(b, 0x31); byte(b, 0xc0); epilog(b);
    if (b->failed) return;
    distance = b->length - (patch + 4);
    for (j = 0; j < 4; ++j)
        b->data[patch + j] = (unsigned char)(distance >> (8 * j));
}

static void guard_step(Emitter *b, const CodonNode *n) {
    load_context(b);
    byte(b, 0x49); byte(b, 0x83); byte(b, 0x38); byte(b, 0); /* cmp qword [r8],0 */
    require_condition(b, n, BIO_FAULT_BUDGET, 0x75); /* jne */
    byte(b, 0x49); byte(b, 0x83); byte(b, 0x28); byte(b, 1); /* sub qword [r8],1 */
}

static void emit_quarantine(Emitter *b, const CodonNode *n) {
    byte(b, 0x41); byte(b, 0xc7); byte(b, 0x40); byte(b, 8); word32(b, n->index);
    byte(b, 0x41); byte(b, 0xc7); byte(b, 0x40); byte(b, 12); word32(b, BIO_FAULT_QUARANTINE);
    byte(b, 0x31); byte(b, 0xc0); epilog(b);
}

static void propagate_failure(Emitter *b) {
    size_t patch, distance;
    load_context(b);
    byte(b, 0x41); byte(b, 0x83); byte(b, 0x78); byte(b, 8); byte(b, 0xff); /* cmp dword [r8+8],-1 */
    byte(b, 0x74); patch = b->length; byte(b, 0); /* je past error return */
    byte(b, 0x31); byte(b, 0xc0); epilog(b);
    if (b->failed) return;
    distance = b->length - (patch + 1);
    if (distance > 127) { b->failed = 1; return; }
    b->data[patch] = (unsigned char)distance;
}

static void initialize_locals(Emitter *b, const BioGene *g) {
    size_t loop, distance;
    unsigned i;
    if (!g->local_cells) return;
    byte(b, 0x4d); byte(b, 0x89); byte(b, 0xca); /* mov r10,r9 */
    byte(b, 0xb9); word32(b, g->local_cells); /* mov ecx,count */
    byte(b, 0x31); byte(b, 0xc0); /* xor eax,eax */
    loop = b->length;
    guard_step(b, g->head); /* Initialization consumes one step per cell. */
    byte(b, 0x41); byte(b, 0x89); byte(b, 0x02); /* mov [r10],eax */
    byte(b, 0x49); byte(b, 0x83); byte(b, 0xc2); byte(b, 4); /* add r10,4 */
    byte(b, 0xff); byte(b, 0xc9); /* dec ecx */
    byte(b, 0x75); /* jnz loop */
    distance = b->length + 1 - loop;
    if (distance > 128) b->failed = 1;
    byte(b, (unsigned)(0u - (unsigned)distance));
    for (i = 0; i < g->local_count; ++i) {
        const BioLocal *local = &g->locals[i];
        if (local->is_array || !local->initial) continue;
        byte(b, 0x41); byte(b, 0xc7); byte(b, 0x81); /* mov dword [r9+offset],initial */
        word32(b, local->offset * 4u); word32(b, local->initial);
    }
}

/* A parameter is a fixed-length borrowed prefix saved in this stack frame.
   Changing virtual r0 or entering another callee cannot change its bound. */
static void array_length(Emitter *b, const BioLocal *local, int edx) {
    if (local->is_parameter) {
        byte(b, 0x8b); byte(b, edx ? 0x54 : 0x44); byte(b, 0x24); byte(b, 88);
    } else { byte(b, edx ? 0xba : 0xb8); word32(b, local->count); }
}

static void array_base(Emitter *b, const BioLocal *local) {
    if (local->is_parameter) {
        byte(b, 0x4c); byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, 80); /* r9=[rsp+80] */
    } else {
        load_locals(b);
        if (local->offset) {
            byte(b, 0x49); byte(b, 0x81); byte(b, 0xc1); word32(b, local->offset * 4u);
        }
    }
}

static void guard_index(Emitter *b, const CodonNode *n, unsigned r, const BioLocal *local) {
    load_eax(b, r);
    array_length(b, local, 1);
    byte(b, 0x41); byte(b, 0x89); byte(b, 0x40); byte(b, 16); /* mov [r8+16],eax */
    byte(b, 0x41); byte(b, 0x89); byte(b, 0x50); byte(b, 20); /* mov [r8+20],edx */
    byte(b, 0x39); byte(b, 0xd0); /* cmp eax,edx */
    require_condition(b, n, BIO_FAULT_BOUNDS, 0x72); /* unsigned jb rejects negative indices too */
}

static void emit_transfer(Emitter *b, const BioGene *g, const CodonNode *n) {
    const BioLocal *local = &g->locals[n->arg1];
    int reading = n->op_code == OP_READ;
    size_t empty_patch, loop, distance;
    /* R10=source, R11=destination, ECX=count. All are volatile under Windows x64. */
    byte(b, 0x4d); byte(b, 0x8b); byte(b, reading ? 0x50 : 0x58); byte(b, reading ? 24 : 40);
    byte(b, 0x4d); byte(b, 0x85); byte(b, reading ? 0xd2 : 0xdb);
    require_condition(b, n, reading ? BIO_FAULT_INPUT : BIO_FAULT_OUTPUT, 0x75);
    if (reading) { byte(b, 0x41); byte(b, 0x8b); byte(b, 0x48); byte(b, 32); }
    else { byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(n->arg2)); }
    byte(b, 0x41); byte(b, 0x89); byte(b, 0x48); byte(b, 16); /* bad_index=ecx */
    array_length(b, local, 0);
    byte(b, 0x41); byte(b, 0x89); byte(b, 0x40); byte(b, 20);
    byte(b, 0x39); byte(b, 0xc1); /* cmp ecx,array capacity */
    require_condition(b, n, reading ? BIO_FAULT_READ_SIZE : BIO_FAULT_WRITE_SIZE, 0x76);
    if (!reading) {
        byte(b, 0x41); byte(b, 0x8b); byte(b, 0x40); byte(b, 52); /* eax=output capacity */
        byte(b, 0x41); byte(b, 0x89); byte(b, 0x40); byte(b, 20);
        byte(b, 0x39); byte(b, 0xc1); /* cmp ecx,eax */
        require_condition(b, n, BIO_FAULT_OUTPUT_SIZE, 0x76);
    }
    /* The opcode already consumed one step; reserve one more per copied cell. */
    byte(b, 0x49); byte(b, 0x39); byte(b, 0x08); /* cmp [r8],rcx */
    require_condition(b, n, BIO_FAULT_BUDGET, 0x73); /* jae */
    byte(b, 0x49); byte(b, 0x29); byte(b, 0x08); /* sub [r8],rcx */
    if (reading) {
        byte(b, 0x89); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(n->arg2));
    } else {
        byte(b, 0x41); byte(b, 0x89); byte(b, 0x48); byte(b, 48); /* output_count=ecx */
        byte(b, 0x41); byte(b, 0xc7); byte(b, 0x40); byte(b, 56); word32(b, 1);
    }
    array_base(b, local);
    byte(b, 0x4d); byte(b, 0x89); byte(b, reading ? 0xcb : 0xca); /* r11/r10=r9 */
    byte(b, 0x85); byte(b, 0xc9); /* test ecx,ecx */
    byte(b, 0x74); empty_patch = b->length; byte(b, 0);
    loop = b->length;
    byte(b, 0x41); byte(b, 0x8b); byte(b, 0x02); /* mov eax,[r10] */
    byte(b, 0x41); byte(b, 0x89); byte(b, 0x03); /* mov [r11],eax */
    byte(b, 0x49); byte(b, 0x83); byte(b, 0xc2); byte(b, 4);
    byte(b, 0x49); byte(b, 0x83); byte(b, 0xc3); byte(b, 4);
    byte(b, 0xff); byte(b, 0xc9);
    byte(b, 0x75); distance = b->length + 1 - loop;
    if (distance > 128) b->failed = 1;
    byte(b, (unsigned)(0u - (unsigned)distance));
    if (b->failed) return;
    distance = b->length - (empty_patch + 1);
    if (distance > 127) { b->failed = 1; return; }
    b->data[empty_patch] = (unsigned char)distance;
}

static void emit_gene(Emitter *b, BioGene *g) {
    CodonNode *n;
    g->code_offset = (uint32_t)b->length;
    /* Shadow space + registers + context + local base + borrowed view + padding. */
    byte(b, 0x48); byte(b, 0x83); byte(b, 0xec); byte(b, BIO_STACK_SIZE);
    byte(b, 0x4c); byte(b, 0x89); byte(b, 0x44); byte(b, 0x24); byte(b, 64); /* mov [rsp+64],r8 */
    byte(b, 0x4c); byte(b, 0x89); byte(b, 0x4c); byte(b, 0x24); byte(b, 72); /* mov [rsp+72],r9 */
    if (g->has_array) {
        /* Internal CALL passes the checked view in volatile R10/R11D. */
        byte(b, 0x4c); byte(b, 0x89); byte(b, 0x54); byte(b, 0x24); byte(b, 80);
        byte(b, 0x44); byte(b, 0x89); byte(b, 0x5c); byte(b, 0x24); byte(b, 88);
    }
    if (g->arity > 0) {
        byte(b, 0x89); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(0));
    }
    if (g->arity > 1) {
        byte(b, 0x89); byte(b, 0x54); byte(b, 0x24); byte(b, slot(1));
    }
    initialize_locals(b, g);
    for (n = g->head->next; n; n = n->next) {
        if (!n->reachable) continue;
        n->code_offset = (uint32_t)b->length;
        if (n->op_code == OP_LABEL || n->op_code == OP_VAR || n->op_code == OP_ARRAY || n->op_code == OP_PARAM) continue;
        guard_step(b, n);
        switch (n->op_code) {
        case OP_QUARANTINE:
            emit_quarantine(b, n);
            break;
        case OP_READ: case OP_WRITE:
            emit_transfer(b, g, n);
            break;
        case OP_GET:
            load_locals(b);
            byte(b, 0x41); byte(b, 0x8b); byte(b, 0x81); word32(b, g->locals[n->arg2].offset * 4u);
            store_eax(b, n->arg1);
            break;
        case OP_SET:
            load_locals(b); load_eax(b, n->arg2);
            byte(b, 0x41); byte(b, 0x89); byte(b, 0x81); word32(b, g->locals[n->arg1].offset * 4u);
            break;
        case OP_LEN:
            array_length(b, &g->locals[n->arg2], 0);
            store_eax(b, n->arg1);
            break;
        case OP_AGET: case OP_ASET: {
            const BioLocal *local = &g->locals[n->op_code == OP_AGET ? n->arg2 >> 8 : n->arg1];
            guard_index(b, n, n->arg2 & 255u, local);
            array_base(b, local);
            if (n->op_code == OP_AGET) {
                byte(b, 0x41); byte(b, 0x8b); byte(b, 0x84); byte(b, 0x81); /* mov eax,[r9+rax*4+offset] */
                word32(b, 0); store_eax(b, n->arg1);
            } else {
                byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(n->arg2 >> 8));
                byte(b, 0x41); byte(b, 0x89); byte(b, 0x8c); byte(b, 0x81); /* mov [r9+rax*4+offset],ecx */
                word32(b, 0);
            }
            break;
        }
        case OP_LOAD:
            byte(b, 0xc7); byte(b, 0x44); byte(b, 0x24); byte(b, slot(n->arg1));
            word32(b, n->arg2);
            break;
        case OP_MOV:
            load_eax(b, n->arg2); store_eax(b, n->arg1);
            break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
            load_eax(b, n->arg1);
            if (n->op_code == OP_MUL) { byte(b, 0x0f); byte(b, 0xaf); }
            else byte(b, n->op_code == OP_ADD ? 0x03 : n->op_code == OP_SUB ? 0x2b :
                         n->op_code == OP_AND ? 0x23 : n->op_code == OP_OR ? 0x0b : 0x33);
            byte(b, 0x44); byte(b, 0x24); byte(b, slot(n->arg2));
            store_eax(b, n->arg1);
            break;
        case OP_DIV: case OP_MOD:
            load_eax(b, n->arg1);
            byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(n->arg2)); /* mov ecx,[rsp+slot] */
            byte(b, 0x85); byte(b, 0xc9); /* test ecx,ecx */
            require_condition(b, n, BIO_FAULT_DIV_ZERO, 0x75); /* jne */
            /* Widen both i32 inputs before IDIV: even INT32_MIN / -1 fits i64.
               Zero is checked first, so IDIV cannot raise a divide exception. */
            byte(b, 0x48); byte(b, 0x98); /* cdqe */
            byte(b, 0x48); byte(b, 0x63); byte(b, 0xc9); /* movsxd rcx,ecx */
            byte(b, 0x48); byte(b, 0x99); /* cqo */
            byte(b, 0x48); byte(b, 0xf7); byte(b, 0xf9); /* idiv rcx */
            if (n->op_code == OP_DIV) {
                byte(b, 0x48); byte(b, 0x63); byte(b, 0xc8); /* movsxd rcx,eax */
                byte(b, 0x48); byte(b, 0x39); byte(b, 0xc8); /* cmp rax,rcx */
                require_condition(b, n, BIO_FAULT_DIV_OVERFLOW, 0x74); /* je: quotient fits i32 */
            } else { byte(b, 0x89); byte(b, 0xd0); } /* mov eax,edx: remainder */
            store_eax(b, n->arg1);
            break;
        case OP_NOT: case OP_LNOT:
            load_eax(b, n->arg1);
            if (n->op_code == OP_NOT) { byte(b, 0xf7); byte(b, 0xd0); } /* not eax */
            else {
                byte(b, 0x85); byte(b, 0xc0); /* test eax,eax */
                byte(b, 0x0f); byte(b, 0x94); byte(b, 0xc0); /* sete al */
                byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc0); /* movzx eax,al */
            }
            store_eax(b, n->arg1);
            break;
        case OP_LAND: case OP_LOR: case OP_LXOR:
            load_eax(b, n->arg1);
            byte(b, 0x85); byte(b, 0xc0); /* test eax,eax */
            byte(b, 0x0f); byte(b, 0x95); byte(b, 0xc0); /* setne al */
            byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc0); /* movzx eax,al */
            byte(b, 0x83); byte(b, 0x7c); byte(b, 0x24); byte(b, slot(n->arg2)); byte(b, 0);
            byte(b, 0x0f); byte(b, 0x95); byte(b, 0xc1); /* setne cl */
            byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc9); /* movzx ecx,cl */
            byte(b, n->op_code == OP_LAND ? 0x23 : n->op_code == OP_LOR ? 0x0b : 0x33);
            byte(b, 0xc1); /* and/or/xor eax,ecx */
            store_eax(b, n->arg1);
            break;
        case OP_SHL: case OP_SHR: case OP_SAR:
            load_eax(b, n->arg1);
            byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(n->arg2));
            byte(b, 0x83); byte(b, 0xf9); byte(b, 31); /* cmp ecx,31 */
            require_condition(b, n, BIO_FAULT_SHIFT, 0x76); /* jbe: unsigned also rejects negatives */
            byte(b, 0xd3); byte(b, n->op_code == OP_SHL ? 0xe0 : n->op_code == OP_SHR ? 0xe8 : 0xf8);
            store_eax(b, n->arg1);
            break;
        case OP_EQ: case OP_LT: case OP_MIN: case OP_MAX:
            load_eax(b, n->arg1);
            byte(b, 0x3b); byte(b, 0x44); byte(b, 0x24); byte(b, slot(n->arg2)); /* cmp eax,[rsp+slot] */
            if (n->op_code == OP_EQ || n->op_code == OP_LT) {
                byte(b, 0x0f); byte(b, n->op_code == OP_EQ ? 0x94 : 0x9c); byte(b, 0xc0); /* sete/setl al */
                byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc0); /* movzx eax,al */
            } else {
                byte(b, 0x0f); byte(b, n->op_code == OP_MIN ? 0x4f : 0x4c); /* cmovg/cmovl eax,[rsp+slot] */
                byte(b, 0x44); byte(b, 0x24); byte(b, slot(n->arg2));
            }
            store_eax(b, n->arg1);
            break;
        case OP_CALL: {
            unsigned argc = n->arg2 >> 16;
            if (n->call_array) {
                const BioLocal *local = &g->locals[n->call_array - 1u];
                load_eax(b, n->arg2 & 255u);
                array_length(b, local, 1);
                byte(b, 0x41); byte(b, 0x89); byte(b, 0x40); byte(b, 16);
                byte(b, 0x41); byte(b, 0x89); byte(b, 0x50); byte(b, 20);
                byte(b, 0x39); byte(b, 0xd0); /* cmp eax,edx: requested length <= capacity */
                require_condition(b, n, BIO_FAULT_ARGUMENT_SIZE, 0x76);
                byte(b, 0x41); byte(b, 0x89); byte(b, 0xc3); /* r11d=length */
                array_base(b, local);
                byte(b, 0x4d); byte(b, 0x89); byte(b, 0xca); /* r10=array */
            }
            load_locals(b);
            if (g->local_cells) {
                byte(b, 0x49); byte(b, 0x81); byte(b, 0xc1); word32(b, g->local_cells * 4u); /* add r9,own cells */
            }
            if (argc > 0) {
                byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, slot(n->arg2 & 255u));
            } else { byte(b, 0x31); byte(b, 0xc9); }
            if (argc > 1) {
                byte(b, 0x8b); byte(b, 0x54); byte(b, 0x24); byte(b, slot((n->arg2 >> 8) & 255u));
            } else { byte(b, 0x31); byte(b, 0xd2); }
            byte(b, 0xe8);
            n->patch_offset = (uint32_t)b->length;
            word32(b, 0);
            propagate_failure(b);
            store_eax(b, 0);
            break;
        }
        case OP_RET:
            load_eax(b, n->arg1);
            epilog(b);
            break;
        case OP_JMP:
            byte(b, 0xe9); n->patch_offset = (uint32_t)b->length; word32(b, 0);
            break;
        case OP_JZ: case OP_JNZ:
            byte(b, 0x83); byte(b, 0x7c); byte(b, 0x24); byte(b, slot(n->arg1)); byte(b, 0);
            byte(b, 0x0f); byte(b, n->op_code == OP_JZ ? 0x84 : 0x85);
            n->patch_offset = (uint32_t)b->length; word32(b, 0);
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
    /* WRITE includes five near guards and a bounded copy loop; every opcode fits in 288 bytes. */
    p->allocation_size = (p->stats.nodes + p->gene_count) * 288u + 16u;
    p->code = VirtualAlloc(NULL, p->allocation_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p->code) { bio_error(e, "VirtualAlloc failed (%lu)", GetLastError()); return 0; }
    b.data = p->code; b.capacity = p->allocation_size;
    for (i = 0; i < p->gene_count; ++i)
        if (p->genes[i].live) emit_gene(&b, &p->genes[i]);
    if (b.failed) { bio_error(e, "internal error: JIT output exceeds reserved buffer"); return 0; }
    p->stats.code_bytes = b.length;
    p->stats.entry_offset = p->genes[p->main_gene].code_offset;
    for (i = 0; i < p->gene_count; ++i) {
        CodonNode *n;
        if (!p->genes[i].live) continue;
        for (n = p->genes[i].head->next; n; n = n->next) {
            uint32_t delta;
            unsigned j;
            if (!n->reachable) continue;
            if (n->op_code == OP_CALL) delta = p->genes[n->arg1].code_offset - (n->patch_offset + 4u);
            else if (n->op_code == OP_JMP || n->op_code == OP_JZ || n->op_code == OP_JNZ)
                delta = n->bond->code_offset - (n->patch_offset + 4u);
            else continue;
            for (j = 0; j < 4; ++j)
                p->code[n->patch_offset + j] = (unsigned char)(delta >> (8u * j));
        }
    }
    while (b.length % 4) byte(&b, 0);
    unwind_offset = (uint32_t)b.length;
    /* UNWIND_INFO v1; prolog 4 bytes; UWOP_ALLOC_SMALL matches the frame. */
    byte(&b, 1); byte(&b, 4); byte(&b, 1); byte(&b, 0);
    byte(&b, 4); byte(&b, ((BIO_STACK_SIZE / 8u - 1u) << 4) | 2u); byte(&b, 0); byte(&b, 0);
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
