#ifndef BIO_INTERNAL_H
#define BIO_INTERNAL_H

#include "bio.h"
#include <windows.h>

#if !defined(_M_X64) && !defined(__x86_64__)
#error Codons JIT requires the Windows x86-64 architecture
#endif

#define BIO_MAX_GENES 256u
#define BIO_MAX_SYMBOLS 512u
#define BIO_MAX_FILES 64u
#define BIO_MAX_NODES 65536u
#define BIO_MAX_DEPTH 64u
#define BIO_IMPORT_DEPTH 16u
#define BIO_LINE_SIZE 512u
#define BIO_PATH_SIZE 1024u
#define BIO_NAME_SIZE 64u
#define BIO_NODES_PER_BLOCK 256u
#define BIO_MAX_SOURCE_BYTES (4u * 1024u * 1024u)
#define BIO_MAX_LOCALS 64u
#define BIO_MAX_LOCAL_CELLS 65536u
#define BIO_MEMORY_CELLS 262144u /* 1 MiB of i32 storage across an active call chain. */
#define BIO_STACK_SIZE 88u

typedef enum {
    OP_GENE, OP_LOAD, OP_MOV, OP_ADD, OP_SUB, OP_MUL, OP_CALL, OP_RET,
    OP_EQ, OP_LT, OP_MIN, OP_MAX, OP_LABEL, OP_JMP, OP_JZ, OP_JNZ,
    OP_DIV, OP_MOD, OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_LAND, OP_LOR, OP_LXOR, OP_LNOT, OP_SHL, OP_SHR, OP_SAR,
    OP_VAR, OP_ARRAY, OP_GET, OP_SET, OP_AGET, OP_ASET, OP_LEN
} BioOp;

typedef struct CodonNode {
    uint8_t op_code;
    uint8_t reachable;
    uint8_t initialized;
    uint8_t queued;
    uint32_t arg1;
    uint32_t arg2;
    struct CodonNode *next;
    struct CodonNode *bond;
    uint32_t line;
    uint32_t file;
    uint32_t patch_offset;
    uint32_t code_offset;
    uint32_t index;
} CodonNode;

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    CodonNode nodes[BIO_NODES_PER_BLOCK];
} ArenaBlock;

typedef struct {
    uint32_t symbol;
    uint32_t offset; /* i32 cells from this call's local base. */
    uint32_t count;
    uint32_t initial;
    unsigned is_array;
} BioLocal;

typedef struct {
    char name[BIO_NAME_SIZE];
    unsigned arity;
    unsigned live;
    unsigned visit;
    unsigned depth;
    uint64_t execution_steps;
    unsigned dynamic_steps;
    CodonNode *head;
    CodonNode *tail;
    uint32_t code_offset;
    uint32_t code_end;
    BioLocal locals[BIO_MAX_LOCALS];
    unsigned local_count;
    uint32_t local_cells;
    uint32_t memory_cells; /* Maximum simultaneously active storage, including callees. */
} BioGene;

typedef struct {
    char path[BIO_PATH_SIZE];
    unsigned state; /* 1: currently parsing, 2: complete */
} BioFile;

/* Each invocation owns one context; nested calls share it, parallel runs do not. */
typedef enum {
    BIO_FAULT_NONE, BIO_FAULT_BUDGET, BIO_FAULT_DIV_ZERO,
    BIO_FAULT_DIV_OVERFLOW, BIO_FAULT_SHIFT, BIO_FAULT_BOUNDS
} BioFault;

typedef struct {
    uint64_t remaining;
    uint32_t failed_node;
    uint32_t fault;
    int32_t bad_index;
    uint32_t bound;
} BioExecution;

_Static_assert(offsetof(BioExecution, remaining) == 0, "JIT budget offset");
_Static_assert(offsetof(BioExecution, failed_node) == 8, "JIT fault offset");
_Static_assert(offsetof(BioExecution, fault) == 12, "JIT fault kind offset");
_Static_assert(offsetof(BioExecution, bad_index) == 16, "JIT array index offset");
_Static_assert(offsetof(BioExecution, bound) == 20, "JIT array bound offset");

struct BioProgram {
    volatile LONG references;
    ArenaBlock *arena;
    BioGene genes[BIO_MAX_GENES];
    BioFile files[BIO_MAX_FILES];
    char symbols[BIO_MAX_SYMBOLS][BIO_NAME_SIZE];
    unsigned gene_count;
    unsigned file_count;
    unsigned symbol_count;
    unsigned main_gene;
    size_t source_bytes;
    BioStats stats;
    unsigned char *code;
    size_t allocation_size;
    RUNTIME_FUNCTION *unwind_table;
    int unwind_registered;
};

void bio_error(BioError *error, const char *format, ...);
void bio_node_error(const BioProgram *p, const CodonNode *n, BioError *e, const char *format, ...);
CodonNode *bio_node_new(BioProgram *p, BioError *e);
void bio_program_retain(BioProgram *p);
int bio_parse(BioProgram *p, const char *path, int repair, BioError *e);
int bio_analyze(BioProgram *p, BioError *e);
int bio_jit(BioProgram *p, BioError *e);
int bio_find_gene(const BioProgram *p, const char *name);
int bio_wide_path(const char *path, wchar_t *out, size_t capacity);
FILE *bio_fopen(const char *path, const wchar_t *mode);

#endif
