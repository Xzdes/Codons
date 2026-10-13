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
#define BIO_MAX_REPAIRS 1024u
#define BIO_MAX_DEPTH 256u /* Active call frames at run time; recursion included. */
#define BIO_IMPORT_DEPTH 16u
#define BIO_LINE_SIZE 512u
#define BIO_PATH_SIZE 1024u
#define BIO_NAME_SIZE 64u
#define BIO_NODES_PER_BLOCK 256u
#define BIO_MAX_SOURCE_BYTES (4u * 1024u * 1024u)
#define BIO_MAX_LOCALS 64u
#define BIO_MAX_LOCAL_CELLS 65536u
#define BIO_MEMORY_CELLS 262144u /* 1 MiB of i32 storage across an active call chain.
    For recursive programs this is the fixed frame pool, checked at every CALL. */
/* Frame: shadow 0-31, outgoing args 32/40, PARAM view 48/56, wide registers
   w0-w3 at 64+8w. Virtual registers rN live in the locals pool instead:
   [rsi + 4*(reg_base + N)], so their count is limited only by the frame
   memory budget. 104 keeps the body 16-aligned for calls and fits
   UWOP_ALLOC_SMALL. */
#define BIO_STACK_SIZE 104u
#define BIO_MAX_LITERALS 1024u
#define BIO_LITERAL_BYTES (256u * 1024u)
#define BIO_MAX_STRINGS 4096u /* Handle table capacity: literals plus dynamic entries. */
#define BIO_STRING_BYTES (512u * 1024u) /* Dynamic pool: below the step budget, so reachable. */

typedef enum {
    OP_GENE, OP_LOAD, OP_MOV, OP_ADD, OP_SUB, OP_MUL, OP_CALL, OP_RET,
    OP_EQ, OP_LT, OP_MIN, OP_MAX, OP_LABEL, OP_JMP, OP_JZ, OP_JNZ,
    OP_DIV, OP_MOD, OP_AND, OP_OR, OP_XOR, OP_NOT,
    OP_LAND, OP_LOR, OP_LXOR, OP_LNOT, OP_SHL, OP_SHR, OP_SAR,
    OP_VAR, OP_ARRAY, OP_GET, OP_SET, OP_AGET, OP_ASET, OP_LEN, OP_READ, OP_WRITE, OP_PARAM,
    OP_QUARANTINE,
    OP_SLOAD, OP_SLEN, OP_SGET, OP_SCAT, OP_SEQ, OP_PRINT,
    /* Wide (i64) registers w0..w3: a class separate from r0..r255, like strings.
       WLOAD carries an index into the program's wide literal table. */
    OP_WLOAD, OP_WMOV, OP_WADD, OP_WSUB, OP_WMUL, OP_WDIV, OP_WMOD,
    OP_WAND, OP_WOR, OP_WXOR, OP_WNOT, OP_WSHL, OP_WSHR, OP_WSAR,
    OP_WEQ, OP_WLT, OP_WMIN, OP_WMAX, OP_WIDEN, OP_NARROW
} BioOp;

typedef struct CodonNode {
    uint8_t op_code;
    uint8_t reachable;
    uint8_t queued;
    uint8_t arg2_literal; /* Binary i32 ops: arg2 is an i32 literal, not a register. */
    uint32_t flow; /* Word offset of this node's init bitset in the validate_flow arena. */
    uint32_t arg1;
    uint32_t arg2;
    uint32_t call_array; /* CALL only: zero or array symbol/local index + 1 after linking. */
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
    unsigned is_parameter; /* Borrowed prefix; no cells owned by this gene. */
} BioLocal;

typedef struct {
    char name[BIO_NAME_SIZE];
    unsigned arity;
    unsigned has_array;
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
    uint32_t reg_base;   /* Registers are locals too: rN lives at cell reg_base + N. */
    uint32_t reg_count;  /* Highest rN mentioned plus one, at least the arity. */
    uint32_t local_cells; /* reg_base + reg_count: the whole frame, registers included. */
    uint32_t memory_cells; /* Maximum simultaneously active storage, including callees. */
} BioGene;

typedef struct {
    char path[BIO_PATH_SIZE];
    unsigned state; /* 1: currently parsing, 2: complete */
} BioFile;

/* Each invocation owns one context; nested calls share it, parallel runs do not. */
typedef enum {
    BIO_FAULT_NONE, BIO_FAULT_BUDGET, BIO_FAULT_DIV_ZERO,
    BIO_FAULT_DIV_OVERFLOW, BIO_FAULT_SHIFT, BIO_FAULT_BOUNDS,
    BIO_FAULT_INPUT, BIO_FAULT_OUTPUT, BIO_FAULT_READ_SIZE,
    BIO_FAULT_WRITE_SIZE, BIO_FAULT_OUTPUT_SIZE, BIO_FAULT_ARGUMENT_SIZE, BIO_FAULT_QUARANTINE,
    BIO_FAULT_STRING_HANDLE, BIO_FAULT_STRING_BUDGET, BIO_FAULT_STRING_BOUNDS,
    BIO_FAULT_STRING_TABLE, BIO_FAULT_PRINT_CAPABILITY, BIO_FAULT_PRINT_FAILED,
    BIO_FAULT_DEPTH, BIO_FAULT_FRAME,
    BIO_FAULT_WIDE_OVERFLOW, BIO_FAULT_WSHIFT, BIO_FAULT_NARROW
} BioFault;

/* Immutable byte string. Literals point into the program's literal pool;
   SCAT results point into the invocation's bump pool. Registers hold handles
   (i32 indices into the invocation's string table). */
typedef struct {
    const uint8_t *data;
    uint32_t length;
} BioString;

typedef struct {
    uint32_t file;
    uint32_t line;
    char kind[24];
    char detail[128];
} BioRepairEntry;

typedef struct {
    uint64_t remaining;
    uint32_t failed_node;
    uint32_t fault;
    int32_t bad_index;
    uint32_t bound;
    const int32_t *input;
    uint32_t input_count;
    uint32_t reserved;
    int32_t *output;
    uint32_t output_count;
    uint32_t output_capacity;
    uint32_t output_written;
    BioString *strings;    /* Handle table: [0, literal_count) literals, then dynamic. */
    uint32_t string_count;
    uint32_t reserved2;
    uint8_t *string_pool;  /* Bump allocator for SCAT results; never freed mid-run. */
    uint32_t pool_used;
    uint32_t pool_capacity;
    /* Host capability for PRINT: NULL forbids printing in this invocation. */
    int (*print)(void *user, const unsigned char *bytes, size_t length);
    void *print_user;
    /* Call frame accounting: frame_used cells are live below the current gene,
       depth counts active genes. Both backends check and bump these at CALL. */
    uint32_t frame_used;
    uint32_t depth;
    uint32_t frame_cells;
    uint32_t reserved3;
} BioExecution;

_Static_assert(offsetof(BioExecution, remaining) == 0, "JIT budget offset");
_Static_assert(offsetof(BioExecution, failed_node) == 8, "JIT fault offset");
_Static_assert(offsetof(BioExecution, fault) == 12, "JIT fault kind offset");
_Static_assert(offsetof(BioExecution, bad_index) == 16, "JIT array index offset");
_Static_assert(offsetof(BioExecution, bound) == 20, "JIT array bound offset");
_Static_assert(offsetof(BioExecution, input) == 24, "JIT input pointer offset");
_Static_assert(offsetof(BioExecution, input_count) == 32, "JIT input count offset");
_Static_assert(offsetof(BioExecution, output) == 40, "JIT output pointer offset");
_Static_assert(offsetof(BioExecution, output_count) == 48, "JIT output count offset");
_Static_assert(offsetof(BioExecution, output_capacity) == 52, "JIT output capacity offset");
_Static_assert(offsetof(BioExecution, output_written) == 56, "JIT output flag offset");
_Static_assert(offsetof(BioExecution, strings) == 64, "JIT string table offset");
_Static_assert(offsetof(BioExecution, string_count) == 72, "JIT string count offset");
_Static_assert(offsetof(BioExecution, string_pool) == 80, "JIT string pool offset");
_Static_assert(offsetof(BioExecution, pool_used) == 88, "JIT pool used offset");
_Static_assert(offsetof(BioExecution, pool_capacity) == 92, "JIT pool capacity offset");
_Static_assert(offsetof(BioExecution, print) == 96, "JIT print capability offset");
_Static_assert(offsetof(BioExecution, print_user) == 104, "JIT print user offset");
_Static_assert(offsetof(BioExecution, frame_used) == 112, "JIT frame used offset");
_Static_assert(offsetof(BioExecution, depth) == 116, "JIT call depth offset");
_Static_assert(offsetof(BioExecution, frame_cells) == 120, "JIT frame capacity offset");

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
    unsigned repair_mode;
    size_t source_bytes;
    size_t repair_count;
    BioRepairEntry repair_log[BIO_MAX_REPAIRS];
    BioStats stats;
    BioString *literals;     /* Compile-time string literals (SLOAD). */
    uint32_t literal_count;
    uint8_t *literal_pool;
    size_t literal_pool_used;
    int64_t *wide_literals;  /* Compile-time i64 literals (WLOAD). */
    uint32_t wide_literal_count;
    unsigned uses_strings;   /* Any string opcode or literal in the program. */
    unsigned has_recursion;  /* A call cycle exists: frame pool is runtime-checked. */
    unsigned char *code;
    size_t allocation_size;
    RUNTIME_FUNCTION *unwind_table;
    int unwind_registered;
};

void bio_error(BioError *error, const char *format, ...);
/* PRINT semantics shared by both backends: capability, handle, budget, callback.
   On failure the fault fields are written and 0 is returned. */
int bio_print_helper(BioExecution *execution, uint32_t handle, uint32_t node);
void bio_node_error(const BioProgram *p, const CodonNode *n, BioError *e, const char *format, ...);
int bio_record_repair(BioProgram *p, const CodonNode *n, const char *kind, const char *detail, BioError *e);
int bio_quarantine(BioProgram *p, CodonNode *n, const char *detail, BioError *e);
CodonNode *bio_node_new(BioProgram *p, BioError *e);
void bio_program_retain(BioProgram *p);
int bio_parse(BioProgram *p, const char *path, int repair, BioError *e);
int bio_analyze(BioProgram *p, BioError *e);
int bio_jit(BioProgram *p, BioError *e);
int bio_find_gene(const BioProgram *p, const char *name);
int bio_wide_path(const char *path, wchar_t *out, size_t capacity);
FILE *bio_fopen(const char *path, const wchar_t *mode);

#endif
