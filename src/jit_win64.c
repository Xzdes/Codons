#include "internal.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
    unsigned char *data;
    size_t length;
    size_t capacity;
    int failed;
    const BioGene *gene; /* Being emitted; register base comes from it. */
    /* Peephole: which virtual register EAX currently holds, -1 for none.
       Only single-step fall-through edges keep the value; labels reset it. */
    int cached;
} Emitter;

static void byte(Emitter *b, unsigned value) {
    if (b->length == b->capacity) { b->failed = 1; return; }
    b->data[b->length++] = (unsigned char)value;
}

static void word32(Emitter *b, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4; ++i) byte(b, (value >> (8 * i)) & 255u);
}

/* Frame layout (BIO_STACK_SIZE bytes above the saved registers):
     0..31  shadow space
    32/40   outgoing context / locals base for nested CALLs (Win64 stack args 5-6)
    48/56   PARAM array view (pointer and length)
    64..95  wide registers w0..w3, 8 bytes each
   Virtual registers rN live in the locals pool instead: [rsi + 4*(reg_base + N)].
   The execution context lives in RBX and the local data base in RSI for the whole
   gene body: both are callee-saved, so nested CALLs (which re-save them) preserve
   the caller's values. */
static unsigned slot(const Emitter *b, unsigned r) { return 4u * (b->gene->reg_base + r); }

/* ModRM + displacement for [rsi + disp] with the given register field. */
static void modrm_rsi(Emitter *b, unsigned reg_field, unsigned disp) {
    if (disp < 128) { byte(b, 0x46u | (reg_field << 3)); byte(b, disp); }
    else { byte(b, 0x86u | (reg_field << 3)); word32(b, disp); }
}

/* Wide i64 registers w0-w3 live in 8-byte frame slots; all accesses use REX.W. */
static unsigned wslot(unsigned w) { return 64u + w * 8u; }
static void load_wide(Emitter *b, unsigned w) {
    byte(b, 0x48); byte(b, 0x8b); byte(b, 0x44); byte(b, 0x24); byte(b, wslot(w)); /* mov rax,[wslot] */
    b->cached = -1;
}
static void store_wide(Emitter *b, unsigned w) {
    byte(b, 0x48); byte(b, 0x89); byte(b, 0x44); byte(b, 0x24); byte(b, wslot(w)); /* mov [wslot],rax */
    b->cached = -1;
}

static void load_eax(Emitter *b, unsigned r) {
    byte(b, 0x8b); modrm_rsi(b, 0, slot(b, r)); /* mov eax,[r] */
    b->cached = (int)r;
}

static void emit_load(Emitter *b, unsigned r) {
    if (b->cached != (int)r) load_eax(b, r);
}

static void store_eax(Emitter *b, unsigned r) {
    byte(b, 0x89); modrm_rsi(b, 0, slot(b, r)); /* mov [r],eax */
    b->cached = (int)r;
}

/* add rsp,BIO_STACK_SIZE; pop rsi; pop rbx; ret */
static void epilog(Emitter *b) {
    byte(b, 0x48); byte(b, 0x83); byte(b, 0xc4); byte(b, BIO_STACK_SIZE);
    byte(b, 0x5e); byte(b, 0x5b); byte(b, 0xc3);
}

/* Jump past a normal error return when the x86 condition is satisfied.
   RBX points to this invocation's context for the whole gene body.
   Near (rel32) form: guards stay valid however large the function grows. */
static void require_condition(Emitter *b, const CodonNode *n, BioFault fault, unsigned condition) {
    size_t patch, distance, j;
    byte(b, 0x0f); byte(b, (condition & 0x0fu) | 0x80u); patch = b->length; word32(b, 0);
    byte(b, 0xc7); byte(b, 0x43); byte(b, 8); word32(b, n->index); /* mov [rbx+8],index */
    byte(b, 0xc7); byte(b, 0x43); byte(b, 12); word32(b, (uint32_t)fault); /* mov [rbx+12],fault */
    byte(b, 0x31); byte(b, 0xc0); epilog(b);
    if (b->failed) return;
    distance = b->length - (patch + 4);
    for (j = 0; j < 4; ++j)
        b->data[patch + j] = (unsigned char)(distance >> (8 * j));
}

static void guard_step(Emitter *b, const CodonNode *n) {
    byte(b, 0x48); byte(b, 0x83); byte(b, 0x3b); byte(b, 0); /* cmp qword [rbx],0 */
    require_condition(b, n, BIO_FAULT_BUDGET, 0x75); /* jne */
    byte(b, 0x48); byte(b, 0x83); byte(b, 0x2b); byte(b, 1); /* sub qword [rbx],1 */
}

static void emit_quarantine(Emitter *b, const CodonNode *n) {
    byte(b, 0xc7); byte(b, 0x43); byte(b, 8); word32(b, n->index);
    byte(b, 0xc7); byte(b, 0x43); byte(b, 12); word32(b, BIO_FAULT_QUARANTINE);
    byte(b, 0x31); byte(b, 0xc0); epilog(b);
    b->cached = -1;
}

static void propagate_failure(Emitter *b) {
    size_t patch, distance;
    byte(b, 0x83); byte(b, 0x7b); byte(b, 8); byte(b, 0xff); /* cmp dword [rbx+8],-1 */
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
    /* Only the declared cells are zeroed (and charged); register cells are
       write-before-read by static proof, matching the interpreter. */
    if (!g->reg_base) return;
    byte(b, 0x49); byte(b, 0x89); byte(b, 0xf2); /* mov r10,rsi */
    byte(b, 0xb9); word32(b, g->reg_base); /* mov ecx,count */
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
        byte(b, 0xc7); byte(b, 0x86); /* mov dword [rsi+offset],initial */
        word32(b, local->offset * 4u); word32(b, local->initial);
    }
    b->cached = -1;
}

/* A parameter is a fixed-length borrowed prefix saved in this stack frame.
   Changing virtual r0 or entering another callee cannot change its bound. */
static void array_length(Emitter *b, const BioLocal *local, int edx) {
    if (local->is_parameter) {
        byte(b, 0x8b); byte(b, edx ? 0x54 : 0x44); byte(b, 0x24); byte(b, 56);
    } else { byte(b, edx ? 0xba : 0xb8); word32(b, local->count); }
}

/* The array base is produced in R9; the length (for PARAM) stays in the frame. */
static void array_base(Emitter *b, const BioLocal *local) {
    if (local->is_parameter) {
        byte(b, 0x4c); byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, 48); /* r9=[rsp+48] */
    } else {
        byte(b, 0x49); byte(b, 0x89); byte(b, 0xf1); /* mov r9,rsi */
        if (local->offset) {
            byte(b, 0x49); byte(b, 0x81); byte(b, 0xc1); word32(b, local->offset * 4u); /* add r9,offset */
        }
    }
}

/* Leaves EAX = zero-extended index and the cache naming that register. */
static void guard_index(Emitter *b, const CodonNode *n, unsigned r, const BioLocal *local) {
    emit_load(b, r);
    array_length(b, local, 1);
    byte(b, 0x89); byte(b, 0x43); byte(b, 16); /* mov [rbx+16],eax */
    byte(b, 0x89); byte(b, 0x53); byte(b, 20); /* mov [rbx+20],edx */
    byte(b, 0x39); byte(b, 0xd0); /* cmp eax,edx */
    require_condition(b, n, BIO_FAULT_BOUNDS, 0x72); /* unsigned jb rejects negative indices too */
}

/* EAX holds a string handle; validate 0 <= handle < string_count (unsigned catch). */
static void guard_handle(Emitter *b, const CodonNode *n) {
    byte(b, 0x89); byte(b, 0x43); byte(b, 16); /* mov [rbx+16],eax: bad_index */
    byte(b, 0x8b); byte(b, 0x4b); byte(b, 72); /* mov ecx,[rbx+72]: count */
    byte(b, 0x89); byte(b, 0x4b); byte(b, 20); /* mov [rbx+20],ecx: bound */
    byte(b, 0x3b); byte(b, 0x43); byte(b, 72); /* cmp eax,[rbx+72] */
    require_condition(b, n, BIO_FAULT_STRING_HANDLE, 0x72); /* jb: valid */
}

/* Loads string A (arg2 & 255) into R10/R11D and string B (arg2 >> 8) into R8/R9D. */
static void load_string_pair(Emitter *b, const CodonNode *n) {
    emit_load(b, n->arg2 & 255u);
    guard_handle(b, n);
    byte(b, 0x48); byte(b, 0x8b); byte(b, 0x53); byte(b, 64); /* mov rdx,[rbx+64]: table */
    byte(b, 0xc1); byte(b, 0xe0); byte(b, 4); /* shl eax,4: 16 bytes per entry */
    byte(b, 0x4c); byte(b, 0x8b); byte(b, 0x14); byte(b, 2); /* mov r10,[rdx+rax] */
    byte(b, 0x44); byte(b, 0x8b); byte(b, 0x5c); byte(b, 2); byte(b, 8); /* mov r11d,[rdx+rax+8] */
    b->cached = -1; /* EAX was shifted; it no longer names a register */
    emit_load(b, n->arg2 >> 8);
    guard_handle(b, n);
    byte(b, 0xc1); byte(b, 0xe0); byte(b, 4);
    byte(b, 0x4c); byte(b, 0x8b); byte(b, 4); byte(b, 2); /* mov r8,[rdx+rax] */
    byte(b, 0x44); byte(b, 0x8b); byte(b, 0x4c); byte(b, 2); byte(b, 8); /* mov r9d,[rdx+rax+8] */
    b->cached = -1;
}

/* A backward rel8 jump to `loop`, computing its own displacement. */
static void jump_back(Emitter *b, unsigned opcode, size_t loop) {
    size_t distance;
    byte(b, opcode);
    distance = b->length + 1 - loop;
    if (distance > 128) b->failed = 1;
    byte(b, (unsigned)(0u - (unsigned)distance));
}

/* A forward rel8 jump; returns the patch offset. */
static size_t jump_ahead(Emitter *b, unsigned opcode) {
    size_t patch;
    byte(b, opcode); patch = b->length; byte(b, 0);
    return patch;
}

static void patch_forward(Emitter *b, size_t patch) {
    size_t distance = b->length - (patch + 1);
    if (distance > 127) { b->failed = 1; return; }
    b->data[patch] = (unsigned char)distance;
}

/* SCAT: validated parts are R10/R11D and R8/R9D. Charges one step per byte. */
static void emit_scat(Emitter *b, const CodonNode *n) {
    size_t loop1, loop2, skip1, skip2;
    load_string_pair(b, n);
    byte(b, 0x44); byte(b, 0x89); byte(b, 0xd9); /* mov ecx,r11d */
    byte(b, 0x44); byte(b, 0x01); byte(b, 0xc9); /* add ecx,r9d: total bytes */
    byte(b, 0x8b); byte(b, 0x43); byte(b, 72); /* mov eax,[rbx+72]: count */
    byte(b, 0x3d); word32(b, BIO_MAX_STRINGS); /* cmp eax,BIO_MAX_STRINGS */
    byte(b, 0xc7); byte(b, 0x43); byte(b, 20); word32(b, BIO_MAX_STRINGS); /* bound = capacity */
    require_condition(b, n, BIO_FAULT_STRING_TABLE, 0x72); /* jb: table has room */
    byte(b, 0x8b); byte(b, 0x43); byte(b, 92); /* mov eax,[rbx+92]: capacity */
    byte(b, 0x2b); byte(b, 0x43); byte(b, 88); /* sub eax,[rbx+88]: remaining */
    byte(b, 0x89); byte(b, 0x4b); byte(b, 16); /* bad_index = needed */
    byte(b, 0x89); byte(b, 0x43); byte(b, 20); /* bound = remaining */
    byte(b, 0x39); byte(b, 0xc1); /* cmp ecx,eax */
    require_condition(b, n, BIO_FAULT_STRING_BUDGET, 0x76); /* jbe: pool has room */
    byte(b, 0x48); byte(b, 0x39); byte(b, 0x0b); /* cmp [rbx],rcx */
    require_condition(b, n, BIO_FAULT_BUDGET, 0x73); /* jae: steps remain */
    byte(b, 0x48); byte(b, 0x29); byte(b, 0x0b); /* sub [rbx],rcx */
    byte(b, 0x48); byte(b, 0x8b); byte(b, 0x53); byte(b, 80); /* mov rdx,[rbx+80]: pool */
    byte(b, 0x8b); byte(b, 0x43); byte(b, 88); /* mov eax,[rbx+88]: used */
    byte(b, 0x48); byte(b, 0x8d); byte(b, 0x14); byte(b, 2); /* lea rdx,[rdx+rax]: bump cursor */
    byte(b, 0x01); byte(b, 0x4b); byte(b, 88); /* add [rbx+88],ecx */
    byte(b, 0x8b); byte(b, 0x43); byte(b, 72); /* mov eax,[rbx+72]: handle = old count */
    store_eax(b, n->arg1); /* EAX survives; the handle is the result */
    byte(b, 0x83); byte(b, 0x43); byte(b, 72); byte(b, 1); /* add dword [rbx+72],1 */
    byte(b, 0xc1); byte(b, 0xe0); byte(b, 4); /* shl eax,4 */
    /* RDI is callee-saved in Win64: the entry address must live in volatile RAX. */
    byte(b, 0x48); byte(b, 0x03); byte(b, 0x43); byte(b, 64); /* add rax,[rbx+64]: &entry */
    byte(b, 0x48); byte(b, 0x89); byte(b, 0x10); /* mov [rax],rdx */
    byte(b, 0x89); byte(b, 0x48); byte(b, 8); /* mov [rax+8],ecx */
    b->cached = -1; /* The copy loops clobber EAX. */
    byte(b, 0x45); byte(b, 0x85); byte(b, 0xdb); /* test r11d,r11d */
    skip1 = jump_ahead(b, 0x74);
    byte(b, 0x31); byte(b, 0xc9); /* xor ecx,ecx */
    loop1 = b->length;
    byte(b, 0x41); byte(b, 0x8a); byte(b, 4); byte(b, 0x0a); /* mov al,[r10+rcx] */
    byte(b, 0x88); byte(b, 4); byte(b, 0x0a); /* mov [rdx+rcx],al */
    byte(b, 0x48); byte(b, 0xff); byte(b, 0xc1); /* inc rcx */
    byte(b, 0x4c); byte(b, 0x39); byte(b, 0xd9); /* cmp rcx,r11 */
    jump_back(b, 0x72, loop1); /* jb */
    patch_forward(b, skip1);
    byte(b, 0x4c); byte(b, 0x01); byte(b, 0xda); /* add rdx,r11: second part follows */
    byte(b, 0x45); byte(b, 0x85); byte(b, 0xc9); /* test r9d,r9d */
    skip2 = jump_ahead(b, 0x74);
    byte(b, 0x31); byte(b, 0xc9); /* xor ecx,ecx */
    loop2 = b->length;
    byte(b, 0x41); byte(b, 0x8a); byte(b, 4); byte(b, 8); /* mov al,[r8+rcx] */
    byte(b, 0x88); byte(b, 4); byte(b, 0x0a); /* mov [rdx+rcx],al */
    byte(b, 0x48); byte(b, 0xff); byte(b, 0xc1); /* inc rcx */
    byte(b, 0x4c); byte(b, 0x39); byte(b, 0xc9); /* cmp rcx,r9 */
    jump_back(b, 0x72, loop2); /* jb */
    patch_forward(b, skip2);
}

/* SEQ: validated parts are R10/R11D and R8/R9D. Charges min(length)+1 steps. */
static void emit_seq(Emitter *b, const CodonNode *n) {
    size_t skip_min, loop, eq, ne1, ne2, done;
    load_string_pair(b, n);
    byte(b, 0x44); byte(b, 0x89); byte(b, 0xd9); /* mov ecx,r11d */
    byte(b, 0x44); byte(b, 0x39); byte(b, 0xc9); /* cmp ecx,r9d */
    skip_min = jump_ahead(b, 0x76); /* jbe: r11d is the minimum */
    byte(b, 0x44); byte(b, 0x89); byte(b, 0xc9); /* mov ecx,r9d */
    patch_forward(b, skip_min);
    byte(b, 0xff); byte(b, 0xc1); /* inc ecx: compare the common prefix plus one */
    byte(b, 0x48); byte(b, 0x39); byte(b, 0x0b); /* cmp [rbx],rcx */
    require_condition(b, n, BIO_FAULT_BUDGET, 0x73); /* jae */
    byte(b, 0x48); byte(b, 0x29); byte(b, 0x0b); /* sub [rbx],rcx */
    byte(b, 0x45); byte(b, 0x39); byte(b, 0xcb); /* cmp r11d,r9d: equal lengths first */
    ne1 = jump_ahead(b, 0x75); /* jne: unequal */
    byte(b, 0x31); byte(b, 0xc9); /* xor ecx,ecx */
    loop = b->length;
    byte(b, 0x4c); byte(b, 0x39); byte(b, 0xd9); /* cmp rcx,r11 */
    eq = jump_ahead(b, 0x73); /* jae: whole prefix matched */
    byte(b, 0x41); byte(b, 0x8a); byte(b, 4); byte(b, 0x0a); /* mov al,[r10+rcx] */
    byte(b, 0x41); byte(b, 0x3a); byte(b, 4); byte(b, 8); /* cmp al,[r8+rcx] */
    ne2 = jump_ahead(b, 0x75); /* jne */
    byte(b, 0x48); byte(b, 0xff); byte(b, 0xc1); /* inc rcx */
    jump_back(b, 0xeb, loop); /* jmp */
    patch_forward(b, eq);
    byte(b, 0xb8); word32(b, 1); /* mov eax,1 */
    store_eax(b, n->arg1);
    done = jump_ahead(b, 0xeb);
    patch_forward(b, ne1); patch_forward(b, ne2);
    byte(b, 0x31); byte(b, 0xc0); /* xor eax,eax */
    store_eax(b, n->arg1);
    patch_forward(b, done);
}

/* PRINT calls the shared C helper under the Win64 ABI: the body keeps RSP
   16-aligned and the frame's shadow space (0..31) is reserved for callees.
   Only volatile registers are used; RBX/RSI survive because the helper is C. */
static void emit_print(Emitter *b, const CodonNode *n) {
    uintptr_t address = (uintptr_t)&bio_print_helper;
    size_t skip;
    unsigned i;
    byte(b, 0x48); byte(b, 0x89); byte(b, 0xd9); /* mov rcx,rbx: execution */
    byte(b, 0x8b); modrm_rsi(b, 2, slot(b, n->arg1)); /* mov edx,[r]: handle */
    byte(b, 0x41); byte(b, 0xb8); word32(b, n->index); /* mov r8d,node */
    byte(b, 0x48); byte(b, 0xb8); /* mov rax,imm64: helper address */
    for (i = 0; i < 8; ++i) byte(b, (unsigned)(address >> (8 * i)) & 255u);
    byte(b, 0xff); byte(b, 0xd0); /* call rax */
    byte(b, 0x85); byte(b, 0xc0); /* test eax,eax */
    skip = jump_ahead(b, 0x75); /* jnz: printed */
    /* The helper already wrote failed_node and the fault kind; just return 0. */
    byte(b, 0x31); byte(b, 0xc0); epilog(b);
    patch_forward(b, skip);
    b->cached = -1; /* The C call clobbers every volatile register. */
}

static void emit_transfer(Emitter *b, const BioGene *g, const CodonNode *n) {
    const BioLocal *local = &g->locals[n->arg1];
    int reading = n->op_code == OP_READ;
    size_t empty_patch, loop, distance;
    /* R10=source, R11=destination, ECX=count. All are volatile under Windows x64. */
    byte(b, 0x4c); byte(b, 0x8b); byte(b, reading ? 0x53 : 0x5b); byte(b, reading ? 24 : 40);
    /*   mov r10,[rbx+24] (input)   or   mov r11,[rbx+40] (output) */
    byte(b, 0x4d); byte(b, 0x85); byte(b, reading ? 0xd2 : 0xdb); /* test r10/r11 */
    require_condition(b, n, reading ? BIO_FAULT_INPUT : BIO_FAULT_OUTPUT, 0x75);
    if (reading) { byte(b, 0x8b); byte(b, 0x4b); byte(b, 32); } /* mov ecx,[rbx+32] */
    else { byte(b, 0x8b); modrm_rsi(b, 1, slot(b, n->arg2)); } /* mov ecx,[r]: count */
    byte(b, 0x89); byte(b, 0x4b); byte(b, 16); /* bad_index=ecx */
    array_length(b, local, 0);
    byte(b, 0x89); byte(b, 0x43); byte(b, 20); /* bound=eax */
    byte(b, 0x39); byte(b, 0xc1); /* cmp ecx,array capacity */
    require_condition(b, n, reading ? BIO_FAULT_READ_SIZE : BIO_FAULT_WRITE_SIZE, 0x76);
    if (!reading) {
        byte(b, 0x8b); byte(b, 0x43); byte(b, 52); /* eax=output capacity */
        byte(b, 0x89); byte(b, 0x43); byte(b, 20); /* bound=eax */
        byte(b, 0x39); byte(b, 0xc1); /* cmp ecx,eax */
        require_condition(b, n, BIO_FAULT_OUTPUT_SIZE, 0x76);
    }
    /* The opcode already consumed one step; reserve one more per copied cell. */
    byte(b, 0x48); byte(b, 0x39); byte(b, 0x0b); /* cmp [rbx],rcx */
    require_condition(b, n, BIO_FAULT_BUDGET, 0x73); /* jae */
    byte(b, 0x48); byte(b, 0x29); byte(b, 0x0b); /* sub [rbx],rcx */
    if (reading) {
        byte(b, 0x89); modrm_rsi(b, 1, slot(b, n->arg2)); /* mov [r],ecx */
    } else {
        byte(b, 0x89); byte(b, 0x4b); byte(b, 48); /* output_count=ecx */
        byte(b, 0xc7); byte(b, 0x43); byte(b, 56); word32(b, 1); /* output_written=1 */
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
    b->cached = -1;
}

static void emit_gene(Emitter *b, const BioProgram *p, BioGene *g) {
    CodonNode *n;
    g->code_offset = (uint32_t)b->length;
    b->gene = g;
    b->cached = -1;
    /* push rbx; push rsi; sub rsp,104. Context and locals base arrive as Win64
       stack arguments 5 and 6: [rsp+160] and [rsp+168] after the prolog. */
    byte(b, 0x53); byte(b, 0x56);
    byte(b, 0x48); byte(b, 0x83); byte(b, 0xec); byte(b, BIO_STACK_SIZE);
    byte(b, 0x48); byte(b, 0x8b); byte(b, 0x9c); byte(b, 0x24); word32(b, 160); /* mov rbx,[rsp+160] */
    byte(b, 0x48); byte(b, 0x8b); byte(b, 0xb4); byte(b, 0x24); word32(b, 168); /* mov rsi,[rsp+168] */
    if (g->has_array) {
        /* Internal CALL passes the checked view in volatile R10/R11D. */
        byte(b, 0x4c); byte(b, 0x89); byte(b, 0x54); byte(b, 0x24); byte(b, 48);
        byte(b, 0x44); byte(b, 0x89); byte(b, 0x5c); byte(b, 0x24); byte(b, 56);
    }
    /* Argument registers land in the callee's own cells: rN is [rsi+4*(reg_base+N)]. */
    if (g->arity > 0) { byte(b, 0x89); modrm_rsi(b, 1, slot(b, 0)); } /* mov [r0],ecx */
    if (g->arity > 1) { byte(b, 0x89); modrm_rsi(b, 2, slot(b, 1)); } /* mov [r1],edx */
    if (g->arity > 2) { byte(b, 0x44); byte(b, 0x89); modrm_rsi(b, 0, slot(b, 2)); } /* mov [r2],r8d */
    if (g->arity > 3) { byte(b, 0x44); byte(b, 0x89); modrm_rsi(b, 1, slot(b, 3)); } /* mov [r3],r9d */
    initialize_locals(b, g);
    for (n = g->head->next; n; n = n->next) {
        if (!n->reachable) continue;
        n->code_offset = (uint32_t)b->length;
        if (n->op_code == OP_LABEL || n->op_code == OP_VAR || n->op_code == OP_ARRAY || n->op_code == OP_PARAM) {
            b->cached = -1; /* LABEL is a join point; the rest share the node. */
            continue;
        }
        guard_step(b, n);
        switch (n->op_code) {
        case OP_QUARANTINE:
            emit_quarantine(b, n);
            break;
        case OP_READ: case OP_WRITE:
            emit_transfer(b, g, n);
            break;
        case OP_GET:
            byte(b, 0x8b); byte(b, 0x86); word32(b, g->locals[n->arg2].offset * 4u); /* mov eax,[rsi+off] */
            store_eax(b, n->arg1);
            break;
        case OP_SLOAD:
            byte(b, 0xb8); word32(b, n->arg2); /* mov eax,literal index */
            store_eax(b, n->arg1);
            break;
        case OP_SLEN:
            emit_load(b, n->arg2);
            guard_handle(b, n);
            byte(b, 0x48); byte(b, 0x8b); byte(b, 0x53); byte(b, 64); /* mov rdx,[rbx+64] */
            byte(b, 0xc1); byte(b, 0xe0); byte(b, 4); /* shl eax,4 */
            byte(b, 0x8b); byte(b, 0x44); byte(b, 2); byte(b, 8); /* mov eax,[rdx+rax+8] */
            store_eax(b, n->arg1);
            break;
        case OP_SGET:
            emit_load(b, n->arg2 & 255u);
            guard_handle(b, n);
            byte(b, 0x48); byte(b, 0x8b); byte(b, 0x53); byte(b, 64); /* mov rdx,[rbx+64] */
            byte(b, 0xc1); byte(b, 0xe0); byte(b, 4); /* shl eax,4 */
            byte(b, 0x4c); byte(b, 0x8b); byte(b, 0x14); byte(b, 2); /* mov r10,[rdx+rax]: data */
            byte(b, 0x44); byte(b, 0x8b); byte(b, 0x5c); byte(b, 2); byte(b, 8); /* mov r11d,[rdx+rax+8]: length */
            byte(b, 0x8b); modrm_rsi(b, 1, slot(b, n->arg2 >> 8)); /* mov ecx,[r]: index */
            byte(b, 0x89); byte(b, 0x4b); byte(b, 16); /* bad_index = index */
            byte(b, 0x44); byte(b, 0x89); byte(b, 0x5b); byte(b, 20); /* bound = length */
            byte(b, 0x44); byte(b, 0x39); byte(b, 0xd9); /* cmp ecx,r11d */
            require_condition(b, n, BIO_FAULT_STRING_BOUNDS, 0x72); /* jb: unsigned rejects negatives too */
            byte(b, 0x41); byte(b, 0x0f); byte(b, 0xb6); byte(b, 4); byte(b, 0x0a); /* movzx eax,byte [r10+rcx] */
            store_eax(b, n->arg1);
            break;
        case OP_SCAT: emit_scat(b, n); break;
        case OP_SEQ: emit_seq(b, n); break;
        case OP_PRINT: emit_print(b, n); break;
        case OP_WLOAD: {
            uint64_t bits;
            unsigned k;
            memcpy(&bits, &p->wide_literals[n->arg2], sizeof(bits));
            byte(b, 0x48); byte(b, 0xb8); /* mov rax,imm64 */
            for (k = 0; k < 8; ++k) byte(b, (unsigned)(bits >> (8 * k)) & 255u);
            store_wide(b, n->arg1);
            break;
        }
        case OP_WMOV: load_wide(b, n->arg2); store_wide(b, n->arg1); break;
        case OP_WADD: case OP_WSUB: case OP_WAND: case OP_WOR: case OP_WXOR:
            load_wide(b, n->arg1);
            byte(b, 0x48);
            byte(b, n->op_code == OP_WADD ? 0x03 : n->op_code == OP_WSUB ? 0x2b :
                   n->op_code == OP_WAND ? 0x23 : n->op_code == OP_WOR ? 0x0b : 0x33);
            byte(b, 0x44); byte(b, 0x24); byte(b, wslot(n->arg2)); /* op rax,[wslot] */
            store_wide(b, n->arg1);
            break;
        case OP_WMUL:
            load_wide(b, n->arg1);
            byte(b, 0x48); byte(b, 0x0f); byte(b, 0xaf); byte(b, 0x44); byte(b, 0x24);
            byte(b, wslot(n->arg2)); /* imul rax,[wslot] */
            store_wide(b, n->arg1);
            break;
        case OP_WDIV: case OP_WMOD: {
            /* 64-bit IDIV raises on INT64_MIN / -1, so check it explicitly. */
            size_t skip;
            unsigned k;
            load_wide(b, n->arg1);
            byte(b, 0x48); byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, wslot(n->arg2)); /* rcx=divisor */
            byte(b, 0x48); byte(b, 0x85); byte(b, 0xc9); /* test rcx,rcx */
            require_condition(b, n, BIO_FAULT_DIV_ZERO, 0x75); /* jne */
            byte(b, 0x48); byte(b, 0x83); byte(b, 0xf9); byte(b, 0xff); /* cmp rcx,-1 */
            skip = jump_ahead(b, 0x75); /* jne: overflow pair impossible */
            byte(b, 0x48); byte(b, 0xba); /* mov rdx,INT64_MIN */
            for (k = 0; k < 8; ++k) byte(b, k == 7 ? 0x80 : 0);
            byte(b, 0x48); byte(b, 0x39); byte(b, 0xd0); /* cmp rax,rdx */
            require_condition(b, n, BIO_FAULT_WIDE_OVERFLOW, 0x75); /* jne: safe */
            patch_forward(b, skip);
            byte(b, 0x48); byte(b, 0x99); /* cqo */
            byte(b, 0x48); byte(b, 0xf7); byte(b, 0xf9); /* idiv rcx */
            if (n->op_code == OP_WMOD) { byte(b, 0x48); byte(b, 0x89); byte(b, 0xd0); } /* mov rax,rdx */
            store_wide(b, n->arg1);
            break;
        }
        case OP_WNOT:
            load_wide(b, n->arg1);
            byte(b, 0x48); byte(b, 0xf7); byte(b, 0xd0); /* not rax */
            store_wide(b, n->arg1);
            break;
        case OP_WSHL: case OP_WSHR: case OP_WSAR:
            byte(b, 0x48); byte(b, 0x8b); byte(b, 0x4c); byte(b, 0x24); byte(b, wslot(n->arg2)); /* rcx=count */
            byte(b, 0x48); byte(b, 0x83); byte(b, 0xf9); byte(b, 63); /* cmp rcx,63 */
            require_condition(b, n, BIO_FAULT_WSHIFT, 0x76); /* jbe: unsigned rejects huge too */
            load_wide(b, n->arg1);
            byte(b, 0x48); byte(b, 0xd3);
            byte(b, n->op_code == OP_WSHL ? 0xe0 : n->op_code == OP_WSHR ? 0xe8 : 0xf8);
            store_wide(b, n->arg1);
            break;
        case OP_WEQ: case OP_WLT:
            load_wide(b, n->arg2 & 255u);
            byte(b, 0x48); byte(b, 0x3b); byte(b, 0x44); byte(b, 0x24);
            byte(b, wslot(n->arg2 >> 8)); /* cmp rax,[wslot] */
            byte(b, 0x0f); byte(b, n->op_code == OP_WEQ ? 0x94 : 0x9c); byte(b, 0xc0); /* sete/setl al */
            byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc0); /* movzx eax,al */
            store_eax(b, n->arg1);
            break;
        case OP_WMIN: case OP_WMAX:
            load_wide(b, n->arg1);
            byte(b, 0x48); byte(b, 0x3b); byte(b, 0x44); byte(b, 0x24); byte(b, wslot(n->arg2));
            byte(b, 0x48); byte(b, 0x0f);
            byte(b, n->op_code == OP_WMIN ? 0x4f : 0x4c); /* cmovg/cmovl rax,[wslot] */
            byte(b, 0x44); byte(b, 0x24); byte(b, wslot(n->arg2));
            store_wide(b, n->arg1);
            break;
        case OP_WIDEN:
            byte(b, 0x48); byte(b, 0x63); modrm_rsi(b, 0, slot(b, n->arg2)); /* movsxd rax,[r] */
            store_wide(b, n->arg1);
            break;
        case OP_NARROW:
            load_wide(b, n->arg2);
            byte(b, 0x48); byte(b, 0x63); byte(b, 0xd0); /* movsxd rdx,eax */
            byte(b, 0x48); byte(b, 0x39); byte(b, 0xc2); /* cmp rdx,rax */
            require_condition(b, n, BIO_FAULT_NARROW, 0x74); /* je: sign extension round-trips */
            store_eax(b, n->arg1);
            break;
        case OP_SET:
            emit_load(b, n->arg2);
            byte(b, 0x89); byte(b, 0x86); word32(b, g->locals[n->arg1].offset * 4u); /* mov [rsi+off],eax */
            break;
        case OP_LEN:
            array_length(b, &g->locals[n->arg2], 0);
            store_eax(b, n->arg1);
            break;
        case OP_AGET: case OP_ASET: {
            const BioLocal *local = &g->locals[n->op_code == OP_AGET ? n->arg2 >> 8 : n->arg1];
            guard_index(b, n, n->arg2 & 255u, local);
            if (local->is_parameter) array_base(b, local);
            if (n->op_code == OP_AGET) {
                if (local->is_parameter) {
                    byte(b, 0x41); byte(b, 0x8b); byte(b, 0x84); byte(b, 0x81); /* mov eax,[r9+rax*4] */
                    word32(b, 0);
                } else {
                    byte(b, 0x8b); byte(b, 0x84); byte(b, 0x86); /* mov eax,[rsi+rax*4+off] */
                    word32(b, local->offset * 4u);
                }
                store_eax(b, n->arg1);
            } else {
                byte(b, 0x8b); modrm_rsi(b, 1, slot(b, n->arg2 >> 8)); /* mov ecx,[r]: value */
                if (local->is_parameter) {
                    byte(b, 0x41); byte(b, 0x89); byte(b, 0x8c); byte(b, 0x81); /* mov [r9+rax*4],ecx */
                    word32(b, 0);
                } else {
                    byte(b, 0x89); byte(b, 0x8c); byte(b, 0x86); /* mov [rsi+rax*4+off],ecx */
                    word32(b, local->offset * 4u);
                }
            }
            break;
        }
        case OP_LOAD:
            byte(b, 0xc7); modrm_rsi(b, 0, slot(b, n->arg1)); /* mov dword [r],imm32 */
            word32(b, n->arg2);
            if (b->cached == (int)n->arg1) b->cached = -1; /* cell rewritten behind EAX */
            break;
        case OP_MOV:
            if (n->arg2_literal) {
                byte(b, 0xb8); word32(b, n->arg2); /* mov eax,imm32 */
            } else emit_load(b, n->arg2);
            store_eax(b, n->arg1);
            break;
        case OP_ADD: case OP_SUB: case OP_MUL: case OP_AND: case OP_OR: case OP_XOR:
            emit_load(b, n->arg1);
            if (n->arg2_literal) {
                if (n->op_code == OP_MUL) { byte(b, 0x69); byte(b, 0xc0); } /* imul eax,eax,imm32 */
                else { /* 81 /digit id: op eax,imm32 */
                    byte(b, 0x81);
                    byte(b, n->op_code == OP_ADD ? 0xc0 : n->op_code == OP_SUB ? 0xe8 :
                             n->op_code == OP_AND ? 0xe0 : n->op_code == OP_OR ? 0xc8 : 0xf0);
                }
                word32(b, n->arg2);
            } else {
                if (n->op_code == OP_MUL) { byte(b, 0x0f); byte(b, 0xaf); }
                else byte(b, n->op_code == OP_ADD ? 0x03 : n->op_code == OP_SUB ? 0x2b :
                             n->op_code == OP_AND ? 0x23 : n->op_code == OP_OR ? 0x0b : 0x33);
                modrm_rsi(b, 0, slot(b, n->arg2)); /* op eax,[r] */
            }
            store_eax(b, n->arg1);
            break;
        case OP_DIV: case OP_MOD:
            emit_load(b, n->arg1);
            if (n->arg2_literal) { /* A zero literal is rejected at compile time. */
                byte(b, 0xb9); word32(b, n->arg2); /* mov ecx,imm32 */
            } else {
                byte(b, 0x8b); modrm_rsi(b, 1, slot(b, n->arg2)); /* mov ecx,[r] */
                byte(b, 0x85); byte(b, 0xc9); /* test ecx,ecx */
                require_condition(b, n, BIO_FAULT_DIV_ZERO, 0x75); /* jne */
            }
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
            emit_load(b, n->arg1);
            if (n->op_code == OP_NOT) { byte(b, 0xf7); byte(b, 0xd0); } /* not eax */
            else {
                byte(b, 0x85); byte(b, 0xc0); /* test eax,eax */
                byte(b, 0x0f); byte(b, 0x94); byte(b, 0xc0); /* sete al */
                byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc0); /* movzx eax,al */
            }
            store_eax(b, n->arg1);
            break;
        case OP_LAND: case OP_LOR: case OP_LXOR:
            emit_load(b, n->arg1);
            byte(b, 0x85); byte(b, 0xc0); /* test eax,eax */
            byte(b, 0x0f); byte(b, 0x95); byte(b, 0xc0); /* setne al */
            byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc0); /* movzx eax,al */
            if (n->arg2_literal) {
                byte(b, 0xb9); word32(b, n->arg2); /* mov ecx,imm32 */
                byte(b, 0x85); byte(b, 0xc9); /* test ecx,ecx */
            } else {
                byte(b, 0x83); modrm_rsi(b, 7, slot(b, n->arg2)); byte(b, 0); /* cmp dword [r],0 */
            }
            byte(b, 0x0f); byte(b, 0x95); byte(b, 0xc1); /* setne cl */
            byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc9); /* movzx ecx,cl */
            byte(b, n->op_code == OP_LAND ? 0x23 : n->op_code == OP_LOR ? 0x0b : 0x33);
            byte(b, 0xc1); /* and/or/xor eax,ecx */
            store_eax(b, n->arg1);
            break;
        case OP_SHL: case OP_SHR: case OP_SAR:
            emit_load(b, n->arg1);
            if (n->arg2_literal) { /* The parser bounded the count to 0..31. */
                byte(b, 0xc1); /* op eax,imm8 */
                byte(b, n->op_code == OP_SHL ? 0xe0 : n->op_code == OP_SHR ? 0xe8 : 0xf8);
                byte(b, (uint8_t)n->arg2);
            } else {
                byte(b, 0x8b); modrm_rsi(b, 1, slot(b, n->arg2)); /* mov ecx,[r]: count */
                byte(b, 0x83); byte(b, 0xf9); byte(b, 31); /* cmp ecx,31 */
                require_condition(b, n, BIO_FAULT_SHIFT, 0x76); /* jbe: unsigned also rejects negatives */
                byte(b, 0xd3); byte(b, n->op_code == OP_SHL ? 0xe0 : n->op_code == OP_SHR ? 0xe8 : 0xf8);
            }
            store_eax(b, n->arg1);
            break;
        case OP_EQ: case OP_LT: case OP_MIN: case OP_MAX:
            emit_load(b, n->arg1);
            if (n->arg2_literal) {
                byte(b, 0x81); byte(b, 0xf8); word32(b, n->arg2); /* cmp eax,imm32 */
            } else {
                byte(b, 0x3b); modrm_rsi(b, 0, slot(b, n->arg2)); /* cmp eax,[r] */
            }
            if (n->op_code == OP_EQ || n->op_code == OP_LT) {
                byte(b, 0x0f); byte(b, n->op_code == OP_EQ ? 0x94 : 0x9c); byte(b, 0xc0); /* sete/setl al */
                byte(b, 0x0f); byte(b, 0xb6); byte(b, 0xc0); /* movzx eax,al */
            } else if (n->arg2_literal) {
                byte(b, 0xba); word32(b, n->arg2); /* mov edx,imm32 (cmov needs a register) */
                byte(b, 0x0f); byte(b, n->op_code == OP_MIN ? 0x4f : 0x4c); byte(b, 0xc2); /* cmov eax,edx */
            } else {
                byte(b, 0x0f); byte(b, n->op_code == OP_MIN ? 0x4f : 0x4c); /* cmovg/cmovl eax,[r] */
                modrm_rsi(b, 0, slot(b, n->arg2));
            }
            store_eax(b, n->arg1);
            break;
        case OP_CALL: {
            /* Argument registers are packed three bits each starting at bit 0. */
            /* The callee's arity says how many 8-bit register fields are live. */
            unsigned argc = p->genes[n->arg1].arity;
            if (n->call_array) {
                const BioLocal *local = &g->locals[n->call_array - 1u];
                emit_load(b, n->arg2 & 255u);
                array_length(b, local, 1);
                byte(b, 0x89); byte(b, 0x43); byte(b, 16);
                byte(b, 0x89); byte(b, 0x53); byte(b, 20);
                byte(b, 0x39); byte(b, 0xd0); /* cmp eax,edx: requested length <= capacity */
                require_condition(b, n, BIO_FAULT_ARGUMENT_SIZE, 0x76);
                byte(b, 0x41); byte(b, 0x89); byte(b, 0xc3); /* r11d=length */
                array_base(b, local);
                byte(b, 0x4d); byte(b, 0x89); byte(b, 0xca); /* r10=array */
            }
            {
                /* Recursion is bounded at run time, after the array size check:
                   depth < BIO_MAX_DEPTH and the callee's cells fit the pool. */
                uint32_t need = p->genes[n->arg1].local_cells;
                byte(b, 0x8b); byte(b, 0x43); byte(b, 116); /* mov eax,[rbx+116]: depth */
                byte(b, 0x3d); word32(b, BIO_MAX_DEPTH); /* cmp eax,BIO_MAX_DEPTH */
                require_condition(b, n, BIO_FAULT_DEPTH, 0x72); /* jb: room for one more */
                byte(b, 0xc7); byte(b, 0x43); byte(b, 16); word32(b, need); /* bad_index = need */
                byte(b, 0x8b); byte(b, 0x4b); byte(b, 120); /* mov ecx,[rbx+120]: capacity */
                byte(b, 0x2b); byte(b, 0x4b); byte(b, 112); /* sub ecx,[rbx+112]: left */
                byte(b, 0x89); byte(b, 0x4b); byte(b, 20); /* bound = cells left */
                byte(b, 0x8b); byte(b, 0x43); byte(b, 112); /* mov eax,[rbx+112]: used */
                byte(b, 0x05); word32(b, need); /* add eax,need */
                byte(b, 0x3b); byte(b, 0x43); byte(b, 120); /* cmp eax,[rbx+120] */
                require_condition(b, n, BIO_FAULT_FRAME, 0x76); /* jbe: fits */
                if (need) {
                    byte(b, 0x81); byte(b, 0x43); byte(b, 112); word32(b, need); /* add [rbx+112],need */
                }
                byte(b, 0xff); byte(b, 0x43); byte(b, 116); /* inc dword [rbx+116] */
                b->cached = -1;
            }
            /* Context and the callee's locals base are Win64 stack arguments 5-6,
               written into the outgoing slots at [rsp+32] and [rsp+40]. */
            byte(b, 0x48); byte(b, 0x89); byte(b, 0x5c); byte(b, 0x24); byte(b, 32); /* mov [rsp+32],rbx */
            byte(b, 0x48); byte(b, 0x89); byte(b, 0xf0); /* mov rax,rsi */
            if (g->local_cells) {
                byte(b, 0x48); byte(b, 0x81); byte(b, 0xc0); word32(b, g->local_cells * 4u); /* add rax,own cells */
            }
            byte(b, 0x48); byte(b, 0x89); byte(b, 0x44); byte(b, 0x24); byte(b, 40); /* mov [rsp+40],rax */
            b->cached = -1;
            if (argc > 0) {
                byte(b, 0x8b); modrm_rsi(b, 1, slot(b, n->arg2 & 255u)); /* mov ecx,[r] */
            } else { byte(b, 0x31); byte(b, 0xc9); } /* xor ecx,ecx */
            if (argc > 1) {
                byte(b, 0x8b); modrm_rsi(b, 2, slot(b, (n->arg2 >> 8) & 255u)); /* mov edx,[r] */
            } else { byte(b, 0x31); byte(b, 0xd2); } /* xor edx,edx */
            if (argc > 2) {
                byte(b, 0x44); byte(b, 0x8b); modrm_rsi(b, 0, slot(b, (n->arg2 >> 16) & 255u)); /* r8d */
            } else { byte(b, 0x45); byte(b, 0x31); byte(b, 0xc0); } /* xor r8d,r8d */
            if (argc > 3) {
                byte(b, 0x44); byte(b, 0x8b); modrm_rsi(b, 1, slot(b, (n->arg2 >> 24) & 255u)); /* r9d */
            } else { byte(b, 0x45); byte(b, 0x31); byte(b, 0xc9); } /* xor r9d,r9d */
            byte(b, 0xe8);
            n->patch_offset = (uint32_t)b->length;
            word32(b, 0);
            {
                /* Unwind on both outcomes: the caller owns the frame accounting. */
                uint32_t need = p->genes[n->arg1].local_cells;
                if (need) {
                    byte(b, 0x81); byte(b, 0x6b); byte(b, 112); word32(b, need); /* sub [rbx+112],need */
                }
                byte(b, 0xff); byte(b, 0x4b); byte(b, 116); /* dec dword [rbx+116] */
            }
            propagate_failure(b);
            store_eax(b, 0);
            break;
        }
        case OP_RET:
            emit_load(b, n->arg1);
            epilog(b);
            b->cached = -1;
            break;
        case OP_JMP:
            byte(b, 0xe9); n->patch_offset = (uint32_t)b->length; word32(b, 0);
            b->cached = -1;
            break;
        case OP_JZ: case OP_JNZ:
            byte(b, 0x83); modrm_rsi(b, 7, slot(b, n->arg1)); byte(b, 0); /* cmp dword [r],0 */
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
        if (p->genes[i].live) emit_gene(&b, p, &p->genes[i]);
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
    /* UNWIND_INFO v1; prolog 6 bytes: push rbx; push rsi; UWOP_ALLOC_SMALL covers
       the 104-byte frame. Codes are listed in undo order; the opinfo nibble of
       UWOP_PUSH_NONVOL is the register number (rsi = 6, rbx = 3). */
    byte(&b, 1); byte(&b, 6); byte(&b, 3); byte(&b, 0);
    byte(&b, 6); byte(&b, ((BIO_STACK_SIZE / 8u - 1u) << 4) | 2u);
    byte(&b, 2); byte(&b, 6u << 4); /* UWOP_PUSH_NONVOL rsi */
    byte(&b, 1); byte(&b, 3u << 4); /* UWOP_PUSH_NONVOL rbx */
    byte(&b, 0); byte(&b, 0); /* alignment pad for the odd code count */
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
