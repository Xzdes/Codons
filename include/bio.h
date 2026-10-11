#ifndef BIO_H
#define BIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define BIO_VERSION "0.4.0"
#define BIO_EXECUTION_LIMIT 1000000u

typedef struct BioProgram BioProgram;
typedef struct BioRuntime BioRuntime;

typedef struct {
    char message[1536];
} BioError;

typedef struct {
    size_t nodes;
    size_t genes;
    size_t live_genes;
    size_t files;
    size_t code_bytes;
    size_t repairs;
    unsigned arity;
    uint64_t execution_steps;
    unsigned dynamic_steps; /* execution_steps is unknown (zero) when this is nonzero. */
    uint32_t entry_offset;
} BioStats;

/* Repair only restores an omitted unused '_' in RET/END. */
BioProgram *bio_compile(const char *path, int repair, BioError *error);
/* Load and validate without allocating executable memory. Paths are UTF-8. */
BioProgram *bio_load(const char *path, int repair, BioError *error);
void bio_program_release(BioProgram *program);
BioStats bio_program_stats(const BioProgram *program);
/* Return 1 on success; on failure return 0 and leave result unchanged.
   arguments may be NULL only when argument_count is zero.
   Each invocation has BIO_EXECUTION_LIMIT steps, shared by its nested calls. */
int bio_program_run(const BioProgram *program, const int32_t *arguments, size_t argument_count,
                    int32_t *result, BioError *error);
int bio_program_interpret(const BioProgram *program, const int32_t *arguments, size_t argument_count,
                          int32_t *result, BioError *error);
void bio_program_dump(const BioProgram *program, FILE *out);
int bio_program_write_code(const BioProgram *program, const char *path, BioError *error);

/* Runtime retains the initial program. Calls and reloads may overlap.
   Destroy only after all calls/reloads have returned. */
BioRuntime *bio_runtime_create(BioProgram *initial, BioError *error);
int bio_runtime_run(BioRuntime *runtime, const int32_t *arguments, size_t argument_count,
                    int32_t *result, BioError *error);
int bio_runtime_reload(BioRuntime *runtime, const char *path, int repair, BioError *error);
void bio_runtime_destroy(BioRuntime *runtime);

#endif
