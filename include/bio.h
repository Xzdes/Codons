#ifndef BIO_H
#define BIO_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

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
} BioStats;

/* Repair only restores an omitted unused '_' in RET/END. */
BioProgram *bio_compile(const char *path, int repair, BioError *error);
void bio_program_release(BioProgram *program);
BioStats bio_program_stats(const BioProgram *program);
int32_t bio_program_run(const BioProgram *program);
int32_t bio_program_interpret(const BioProgram *program);
void bio_program_dump(const BioProgram *program, FILE *out);
int bio_program_write_code(const BioProgram *program, const char *path, BioError *error);

/* Runtime retains the initial program. Calls and reloads may overlap.
   Destroy only after all calls/reloads have returned. */
BioRuntime *bio_runtime_create(BioProgram *initial, BioError *error);
int32_t bio_runtime_run(BioRuntime *runtime);
int bio_runtime_reload(BioRuntime *runtime, const char *path, int repair, BioError *error);
void bio_runtime_destroy(BioRuntime *runtime);

#endif
