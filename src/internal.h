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

typedef enum { OP_GENE, OP_LOAD, OP_MOV, OP_ADD, OP_SUB, OP_MUL, OP_CALL, OP_RET } BioOp;

typedef struct CodonNode {
    uint8_t op_code;
    uint32_t arg1;
    uint32_t arg2;
    struct CodonNode *next;
    struct CodonNode *bond;
    uint32_t line;
    uint32_t file;
    uint32_t patch_offset;
} CodonNode;

typedef struct ArenaBlock {
    struct ArenaBlock *next;
    size_t used;
    CodonNode nodes[BIO_NODES_PER_BLOCK];
} ArenaBlock;

typedef struct {
    char name[BIO_NAME_SIZE];
    unsigned arity;
    unsigned live;
    unsigned visit;
    unsigned depth;
    CodonNode *head;
    CodonNode *tail;
    uint32_t code_offset;
    uint32_t code_end;
} BioGene;

typedef struct {
    char path[BIO_PATH_SIZE];
    unsigned state; /* 1: currently parsing, 2: complete */
} BioFile;

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

#endif
